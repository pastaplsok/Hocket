#pragma once

// Shared setup for the engine tests: one Tracktion Engine for the whole run,
// projects built in code, offline renders, and a fake audio device for testing
// live playback.

#include "core/commands/CommandBus.h"
#include "core/project/Project.h"
#include "engine/EditSync.h"

#include <tracktion_engine/tracktion_engine.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hocket::testing
{
    // Tracktion allows one Engine at a time, so the tests share this one. It
    // never opens a real audio device, and it keeps its settings and temporary
    // files in testFolder(), never in the user's %APPDATA%\Hocket.
    tracktion::Engine& testEngine();

    // A temporary folder with a non-ASCII name, deleted after the run.
    juce::File testFolder();

    //==============================================================================
    // In 4/4.
    inline constexpr notes::Ticks beat = notes::ticksPerQuarterNote;
    inline constexpr notes::Ticks bar = 4 * beat;

    // A clip, and the .notes text of its file; no text for a clip linked to
    // another, which uses that one's file.
    struct TestClip
    {
        project::Clip clip;
        std::optional<std::string> notesText;
    };

    // A clip whose file is clips/<its ID without "clp-">.notes, with this text.
    TestClip makeClip (std::string id, notes::Ticks start, std::string_view notesText);

    // A clip linked to another: it uses that one's file, and so its notes.
    TestClip linkedClip (std::string id, notes::Ticks start, const TestClip& linkedTo);

    struct TestTrack
    {
        project::Track track;
        std::vector<TestClip> clips;
    };

    // A midi track with a hocket:synth (a 4OSC in the Edit) and the given clips.
    TestTrack makeTrack (std::string id, std::string name, std::vector<TestClip> clips);

    // A project with these tracks. Each clip file's text must be valid in this time signature.
    project::Project makeProject (std::vector<TestTrack> tracks, double tempo = 120.0, notes::Meter meter = {});

    inline const commands::Author lee { "Lee" };
    inline const commands::Author claude { "Claude", "test-session" };

    //==============================================================================
    struct Audio
    {
        juce::AudioBuffer<float> buffer;
        double sampleRate = 0.0;

        int sampleAt (double seconds) const { return static_cast<int> (seconds * sampleRate + 0.5); }
    };

    // Sends play through the bus, as the tests do, and times it. A play that
    // takes more than 1.5 s prints a warning: on some days Tracktion's play()
    // has taken 3 s or more (PROGRESS.md, Loose ends). Switches, from the
    // environment, for finding out why:
    //   HOCKET_TEST_TIME_PLAY=1         prints how long every play took
    //   HOCKET_TEST_MIDI_OUTPUTS_OFF=1  disables Tracktion's MIDI outputs first
    //   HOCKET_TEST_SAMPLE_PLAY=1       samples the message thread's stack
    //                                   during the play and prints it (names
    //                                   need the vs2026-symbols build)
    commands::Result timedPlay (commands::CommandBus& bus, const commands::Author& author);

    // Renders the loop range offline, without an audio device.
    Audio renderLoop (engine::EditSync& sync);

    // The first sample at or after `from`, on any channel, louder than the
    // threshold; -1 if there's none.
    int firstSoundAt (const juce::AudioBuffer<float>& buffer, int from = 0, float threshold = 1.0e-3f);

    // The loudest sample in [start, end), on any channel.
    float peakBetween (const juce::AudioBuffer<float>& buffer, int start, int end);

    //==============================================================================
    // Tracktion's hosted audio device: plays through the same live-playback path
    // as a sound card (graph swaps included), but block by block, as fast as the
    // test asks. Open it before creating the EditSync, so its Edit plays here.
    class FakeAudioDevice
    {
    public:
        static constexpr double sampleRate = 44100.0;
        static constexpr int blockSize = 256;

        FakeAudioDevice();
        ~FakeAudioDevice();

        FakeAudioDevice (const FakeAudioDevice&) = delete;
        FakeAudioDevice& operator= (const FakeAudioDevice&) = delete;

        // Plays the next numSamples and adds them to what's been played so far.
        void play (int numSamples);

        // Plays the next block into `destination` from sample `start`, which
        // must leave room for it. Nothing is allocated, so a real-time thread
        // can call it (the stress test does); output() doesn't include it.
        // Returns how long Tracktion took, in seconds.
        double playBlock (juce::AudioBuffer<float>& destination, int start);

        // Everything played since the device opened.
        const Audio& output() const { return played; }

    private:
        tracktion::HostedAudioDeviceInterface& device;
        Audio played;
        juce::MidiBuffer midiScratch; // playBlock's, kept so it never allocates
    };

    //==============================================================================
    // Records the MIDI that reaches the synth: a plugin put ahead of 4OSC on the
    // track. Its storage is allocated up front, so the audio thread never
    // allocates; read it once playback has stopped.
    class MidiRecorder final : public tracktion::Plugin
    {
    public:
        static inline const char* xmlTypeName = "hocketTestMidiRecorder";

        struct Event
        {
            std::int64_t sample = 0; // counted from the first block it saw
            int note = -1;           // -1: all notes off
            bool on = false;
        };

        explicit MidiRecorder (tracktion::PluginCreationInfo info) : Plugin (info) { events.resize (100000); }
        ~MidiRecorder() override { notifyListenersOfDeletion(); }

        // Puts one ahead of everything else on one of the Edit's tracks, the first by default.
        static MidiRecorder& addTo (tracktion::Edit& edit, int trackIndex = 0)
        {
            static const bool registered = [&edit]
            {
                edit.engine.getPluginManager().createBuiltInType<MidiRecorder>();
                return true;
            }();

            auto plugin = edit.getPluginCache().createNewPlugin (xmlTypeName, {});
            REQUIRE ((registered && plugin != nullptr));
            tracktion::getAudioTracks (edit)[trackIndex]->pluginList.insertPlugin (plugin, 0, nullptr);

            // Creating it records an undo step, which EditSync would clear
            // after a change of its own; checkEditMatches expects none.
            edit.getUndoManager().clearUndoHistory();
            return *dynamic_cast<MidiRecorder*> (plugin.get());
        }

        juce::String getName() const override { return "MIDI recorder"; }
        juce::String getPluginType() override { return xmlTypeName; }
        juce::String getSelectableDescription() override { return getName(); }
        BusLayout getBusses() const override { return {}; }
        int getNumOutputChannelsGivenInputs (int) override { return 0; }
        void initialise (const tracktion::PluginInitialisationInfo&) override {}
        void deinitialise() override {}

        void applyToBuffer (const tracktion::PluginRenderContext& context) override
        {
            if (const auto* midi = context.bufferForMidiMessages)
            {
                if (midi->isAllNotesOff)
                    add ({ samples, -1, false });

                for (const auto& m : *midi)
                    if (m.isNoteOnOrOff())
                        add ({ samples + juce::jlimit (0, context.bufferNumSamples - 1, juce::roundToInt (m.getTimeStamp() * FakeAudioDevice::sampleRate)),
                               m.getNoteNumber(), m.isNoteOn() });
            }

            samples += context.bufferNumSamples;
        }

        // In the order the synth gets them. Once playback has stopped.
        std::vector<Event> recorded() const
        {
            std::vector<Event> sorted (events.begin(), events.begin() + count.load());
            std::ranges::stable_sort (sorted, {}, &Event::sample);
            return sorted;
        }

    private:
        std::vector<Event> events;
        std::atomic<int> count { 0 };
        std::int64_t samples = 0;

        void add (Event e)
        {
            if (const auto i = count.load(); i < static_cast<int> (events.size()))
            {
                events[static_cast<std::size_t> (i)] = e;
                count.store (i + 1);
            }
        }
    };

    //==============================================================================
    // A steady tone on both channels, in place of whatever comes in: a sine
    // whose period is exactly `period` samples, at `level`. So the loudest
    // sample in any `period` samples in a row is `level` times the gains after
    // it, exactly: a track's mix can be measured, where 4OSC's level drifts.
    class SteadyTone final : public tracktion::Plugin
    {
    public:
        static inline const char* xmlTypeName = "hocketTestSteadyTone";
        static constexpr int period = 100;
        static constexpr float level = 0.25f;

        explicit SteadyTone (tracktion::PluginCreationInfo info) : Plugin (info) {}
        ~SteadyTone() override { notifyListenersOfDeletion(); }

        // Puts one just ahead of the track's gain stage, which plays its
        // mix, after its synth.
        static SteadyTone& addTo (tracktion::AudioTrack& track)
        {
            static const bool registered = [&track]
            {
                track.edit.engine.getPluginManager().createBuiltInType<SteadyTone>();
                return true;
            }();

            auto plugin = track.edit.getPluginCache().createNewPlugin (xmlTypeName, {});
            const auto stages = track.pluginList.getPluginsOfType<engine::GainStagePlugin>();
            REQUIRE ((registered && plugin != nullptr && stages.size() == 1));
            track.pluginList.insertPlugin (plugin, track.pluginList.indexOf (stages.getFirst()), nullptr);
            return *dynamic_cast<SteadyTone*> (plugin.get());
        }

        juce::String getName() const override { return "Steady tone"; }
        juce::String getPluginType() override { return xmlTypeName; }
        juce::String getSelectableDescription() override { return getName(); }
        BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
        int getNumOutputChannelsGivenInputs (int) override { return 2; }
        void initialise (const tracktion::PluginInitialisationInfo&) override {}
        void deinitialise() override {}

        void applyToBuffer (const tracktion::PluginRenderContext& context) override
        {
            auto* buffer = context.destBuffer;

            if (buffer == nullptr)
                return;

            for (int i = 0; i < context.bufferNumSamples; ++i)
            {
                const auto value = level * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * phase / period));
                phase = (phase + 1) % period;

                for (int channel = 0; channel < buffer->getNumChannels(); ++channel)
                    buffer->setSample (channel, context.bufferStartSample + i, value);
            }
        }

    private:
        int phase = 0; // audio thread only
    };

    // A gain on each channel.
    struct ChannelGains
    {
        double left = 0.0;
        double right = 0.0;
    };

    // What SPEC §4.4 says a track's channels get from its mix: the volume on
    // both, less on the far side when panned (balance law), and nothing when
    // muted, or when another track is soloed and this one isn't.
    ChannelGains specifiedGains (const project::Project&, const project::Track&);

    // What Tracktion makes of a volume plugin's settings, by its own sums.
    ChannelGains volumePluginGains (tracktion::VolumeAndPanPlugin&);

    // The gains a track's gain stage is set to.
    ChannelGains gainStageGains (const engine::GainStagePlugin&);

    // Whether two gains are the same to within a thousandth of a dB, or both silence.
    bool sameGain (double a, double b);

    // The Edit is the bus's project: one tempo, the song's, with the Edit's
    // time signature at 4/4; the tracks in order, their plugins and mix (each
    // track's gain stage, at its place, with Tracktion's volume plugin after
    // it at unity); the
    // clips, in beats and seconds, and their notes; what play plays; and
    // nothing in Tracktion's undo. While the transport plays once (loop off),
    // every clip ends by the end of what plays, and one that starts at or
    // after it isn't there (SPEC §3.1). That's worked out here from the bus,
    // not asked of EditSync, so it can't hide a mismatch.
    void checkEditMatches (engine::EditSync&, const commands::CommandBus&);

    // What the app's message loop does after a change during playback, in a
    // test that plays the device itself: EditSync puts back the notes it held
    // back and takes out the clips it emptied, once a block has played so
    // (EditSync::heldNotes, endingClips). Plays three blocks, running the
    // message loop after the first and the third.
    void letEditSettle (FakeAudioDevice&, engine::EditSync&);

    // The notes the synth is left holding after these events, by JUCE's rules:
    // a note-on for a note already on retriggers it, and a note-off for one
    // that's off is ignored.
    inline std::vector<int> notesLeftOn (const std::vector<MidiRecorder::Event>& events)
    {
        std::array<bool, 128> on {};

        for (const auto& e : events)
        {
            if (e.note < 0)
                on.fill (false);
            else
                on[static_cast<std::size_t> (e.note)] = e.on;
        }

        std::vector<int> left;

        for (int note = 0; note < 128; ++note)
            if (on[static_cast<std::size_t> (note)])
                left.push_back (note);

        return left;
    }

    // The last few events for a note, for a failure message.
    inline std::string historyOf (int note, const std::vector<MidiRecorder::Event>& events, std::size_t last = 8)
    {
        std::vector<std::string> lines;

        for (const auto& e : events)
            if (e.note == note || e.note < 0)
                lines.push_back ((e.note < 0 ? std::string ("all notes off") : std::string (e.on ? "on " : "off ") + std::to_string (note))
                                 + " at sample " + std::to_string (e.sample));

        std::string text;

        for (auto i = lines.size() > last ? lines.size() - last : 0; i < lines.size(); ++i)
            text += lines[i] + "; ";

        return text;
    }
}
