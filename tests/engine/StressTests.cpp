// Milestone 0's stress test (SPEC §10.0): 200 rewrites at 20 a second while
// the song plays cause no xruns and no dropouts.
//
// An audio thread plays the fake device at the pace a sound card asks for
// blocks, with real-time priority, timing each block. Meanwhile the test
// thread is the message thread, as in hocket-headless: it sends the commands
// and runs the message loop between them, where Tracktion rebuilds its
// playback graph and hands it to the audio thread.

#include "EngineTesting.h"

#include "core/notes/Notes.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <sstream>
#include <thread>

using namespace hocket;
using namespace hocket::testing;
using namespace std::chrono_literals;

namespace
{
    using Clock = std::chrono::steady_clock;

    // At 120 BPM and 44.1 kHz.
    constexpr int beatSamples = 22050;
    constexpr int barSamples = 4 * beatSamples;
    constexpr int blockSize = FakeAudioDevice::blockSize;
    constexpr double blockSeconds = blockSize / FakeAudioDevice::sampleRate;

    // Plays the device on a real-time thread, a block each time a sound card
    // would ask for one, into a buffer allocated up front. Plays until told to
    // stop, or until the buffer is full.
    class AudioThread final : private juce::Thread
    {
    public:
        AudioThread (FakeAudioDevice& fakeDevice, double maxSeconds)
            : juce::Thread ("stress test audio"),
              device (fakeDevice),
              capacity (static_cast<int> (maxSeconds / blockSeconds))
        {
            audio.setSize (2, capacity * blockSize);
            processing.resize (static_cast<std::size_t> (capacity));
            REQUIRE (startRealtimeThread ({}));
        }

        ~AudioThread() override { stopThread (10000); }

        int blocksPlayed() const { return played.load(); }

        // Plays up to this many blocks in all, then stops.
        void stopAfter (int blocks) { lastBlock.store (std::min (blocks, capacity)); }

        bool isDone() const { return ! isThreadRunning(); }

        // Once it's done: what it played, and how long each block took Tracktion.
        const juce::AudioBuffer<float>& output() const { return audio; }
        const std::vector<double>& processingSeconds() const { return processing; }

    private:
        FakeAudioDevice& device;
        const int capacity;
        juce::AudioBuffer<float> audio;
        std::vector<double> processing;
        std::atomic<int> played { 0 };
        std::atomic<int> lastBlock { capacity };

        void run() override
        {
            const auto start = Clock::now();

            for (int i = 0; i < lastBlock.load() && ! threadShouldExit(); ++i)
            {
                // Block i is due when the blocks before it would have finished playing.
                std::this_thread::sleep_until (start + std::chrono::duration_cast<Clock::duration> (std::chrono::duration<double> (i * blockSeconds)));
                processing[static_cast<std::size_t> (i)] = device.playBlock (audio, i * blockSize);
                played.store (i + 1);
            }
        }
    };

    // Runs the message loop, as hocket-headless's main thread does, until `done` says so.
    void runMessagesUntil (const std::function<bool()>& done)
    {
        while (! done())
            juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
    }

    // Sends `count` rewrites, one every `interval`, from the bus's point of view
    // as an API client would.
    void rewrite (commands::CommandBus& bus, int count, std::chrono::milliseconds interval, const std::function<std::string (int)>& text)
    {
        const auto start = Clock::now();

        for (int i = 0; i < count; ++i)
        {
            runMessagesUntil ([due = start + i * interval] { return Clock::now() >= due; });
            REQUIRE (bus.execute (commands::WriteClip { "clp-bass", text (i) }, claude).ok);
        }
    }

    struct Timing
    {
        int blocks = 0;
        int overBudget = 0; // took longer than the block lasts: an xrun on a sound card
        double slowest = 0.0;
        double mean = 0.0;
    };

    Timing timing (const std::vector<double>& processing, int firstBlock, int endBlock)
    {
        Timing result;

        for (auto i = firstBlock; i < endBlock; ++i)
        {
            const auto seconds = processing[static_cast<std::size_t> (i)];
            ++result.blocks;
            result.overBudget += seconds > blockSeconds ? 1 : 0;
            result.slowest = std::max (result.slowest, seconds);
            result.mean += seconds;
        }

        result.mean /= std::max (1, result.blocks);
        return result;
    }

    std::string describe (const Timing& t)
    {
        std::ostringstream text;
        text.precision (3);
        text << t.blocks << " blocks, " << t.overBudget << " took longer than the " << blockSeconds * 1000.0 << " ms they last; slowest "
             << t.slowest * 1000.0 << " ms, mean " << t.mean * 1000.0 << " ms";
        return text.str();
    }

    //==============================================================================
    // The held-note checks. In each pass, a note sounds from 0.25 s in (after
    // its attack) to just before the loop ends and cuts it off; that stretch is
    // compared with the same stretch of the first, untouched pass.
    constexpr int checkedFrom = beatSamples / 2;
    constexpr int checkedTo = barSamples - 2048;
    constexpr int window = 882; // 20 ms

    bool isChecked (int sample)
    {
        const auto inPass = sample % barSamples;
        return sample >= barSamples && inPass >= checkedFrom && inPass < checkedTo;
    }

    // Blocks after the first pass that came out completely silent while the
    // note should have been sounding: Tracktion skipped them.
    std::vector<int> droppedBlocks (const juce::AudioBuffer<float>& out, int blocks)
    {
        std::vector<int> dropped;

        for (int b = 0; b < blocks; ++b)
            if (isChecked (b * blockSize) && isChecked ((b + 1) * blockSize - 1) && out.getMagnitude (0, b * blockSize, blockSize) == 0.0f)
                dropped.push_back (b);

        return dropped;
    }

    bool overlapsDropped (int start, int end, const std::vector<int>& dropped)
    {
        return std::ranges::any_of (dropped, [start, end] (int b) { return b * blockSize < end && (b + 1) * blockSize > start; });
    }

    // What's wrong with a pass, compared with the first: a 20 ms window more
    // than 10% quieter or louder (a gap, a retrigger), or a jump from one
    // sample to the next half as big again as any in the first (a click).
    // Dropped blocks are left out, as they're counted on their own. Empty if
    // nothing's wrong.
    std::string problemsInPass (const juce::AudioBuffer<float>& out, int pass, const std::vector<int>& dropped)
    {
        std::ostringstream problems;
        const auto* samples = out.getReadPointer (0);
        float referenceStep = 0.0f, largestStep = 0.0f;

        for (auto i = checkedFrom + 1; i < checkedTo; ++i)
        {
            const auto at = pass * barSamples + i;
            referenceStep = std::max (referenceStep, std::abs (samples[i] - samples[i - 1]));

            if (! overlapsDropped (at - 1, at + 1, dropped))
                largestStep = std::max (largestStep, std::abs (samples[at] - samples[at - 1]));
        }

        if (largestStep > 1.5f * referenceStep)
            problems << "a click: a step of " << largestStep << " between samples, where the first pass's largest is " << referenceStep << ". ";

        for (auto start = checkedFrom; start + window <= checkedTo; start += window / 2)
        {
            const auto at = pass * barSamples + start;

            if (overlapsDropped (at, at + window, dropped))
                continue;

            const auto level = out.getRMSLevel (0, at, window) / out.getRMSLevel (0, start, window);

            if (level < 0.9f || level > 1.1f)
            {
                problems << "the 20 ms from sample " << at << " are at " << level << " of the first pass's loudness. ";
                break;
            }
        }

        return problems.str();
    }

    project::Project oneClip (std::string_view notesText)
    {
        return makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", 0, notesText) }) });
    }

}

TEST_CASE ("stress: 200 rewrites at 20 a second during playback cause no xruns and no dropouts")
{
    // A note held for the whole loop. The rewrites only change a comment, so
    // the music should stay exactly the same: any gap, retrigger or click is
    // the engine's doing. Each rewrite still goes all the way through: a
    // history entry, a new Tracktion sequence, and a graph rebuild and swap.
    const std::string notes = "@length 1bar\n\n1.1 C3 1/1 v100\n";
    constexpr int rewrites = 200;

    FakeAudioDevice device;
    commands::CommandBus bus (oneClip (notes));
    engine::EditSync sync (testEngine(), bus);
    REQUIRE (timedPlay (bus, lee).ok);

    // A pass untouched, for reference, then 10 s of rewrites, then to the end of that pass.
    AudioThread audio (device, 20.0);
    runMessagesUntil ([&audio] { return audio.blocksPlayed() * blockSize >= barSamples; });

    const auto firstBlock = audio.blocksPlayed();
    const auto start = Clock::now();
    rewrite (bus, rewrites, 50ms, [&notes] (int i) { return "# rewrite " + std::to_string (i + 1) + "\n" + notes; });
    const auto rewritesTook = std::chrono::duration<double> (Clock::now() - start).count();

    audio.stopAfter ((audio.blocksPlayed() * blockSize / barSamples + 1) * barSamples / blockSize + 1);
    runMessagesUntil ([&audio] { return audio.isDone(); });

    CHECK (bus.history().size() == rewrites);
    CHECK_MESSAGE (rewritesTook < 10.5, "the rewrites took " << rewritesTook << " s");

    // No xruns: every block was ready in less time than it lasts.
    const auto stressed = timing (audio.processingSeconds(), firstBlock, audio.blocksPlayed());
    MESSAGE ("before the rewrites: " << describe (timing (audio.processingSeconds(), 0, firstBlock)));
    MESSAGE ("during the rewrites: " << describe (stressed));

#ifdef NDEBUG
    // A Debug build of Tracktion isn't meant to keep up in real time.
    CHECK (stressed.overBudget == 0);
#endif

    const auto& out = audio.output();
    const auto passes = audio.blocksPlayed() * blockSize / barSamples;
    REQUIRE (passes >= 6);
    REQUIRE (out.getRMSLevel (0, checkedFrom, window) > 0.01f);

    // No dropped blocks: a whole block of silence while the note should sound.
    // Tracktion used to skip a block now and then, when a graph rebuild held
    // the lock the audio thread needs; Hocket's fork of it fixes that
    // (DEPENDENCIES.md).
    const auto dropped = droppedBlocks (out, audio.blocksPlayed());
    MESSAGE ("dropped blocks: " << dropped.size());
    CHECK_MESSAGE (dropped.empty(), "Tracktion played " << dropped.size() << " block(s) of silence");

    // Anything else, a gap, retrigger or click, fails it.
    for (int pass = 1; pass < passes; ++pass)
    {
        CAPTURE (pass);
        CHECK (problemsInPass (out, pass, dropped).empty());
    }

    // The checks can fail: a dropped block, a click and a dip, put into a copy.
    auto damaged = out;
    const auto silencedBlock = 2 * barSamples / blockSize + 64;
    damaged.clear (0, silencedBlock * blockSize, blockSize);
    damaged.clear (1, silencedBlock * blockSize, blockSize);
    damaged.getWritePointer (0)[3 * barSamples + beatSamples] += 0.25f;
    damaged.applyGain (0, 4 * barSamples + 2 * beatSamples, window, 0.5f);

    CHECK (droppedBlocks (damaged, audio.blocksPlayed()).size() == dropped.size() + 1);
    CHECK (problemsInPass (damaged, 3, dropped).find ("a click") != std::string::npos);
    CHECK (problemsInPass (damaged, 4, dropped).find ("of the first pass's loudness") != std::string::npos);
}

TEST_CASE ("stress: rewrites that move the notes cause no xruns, and the last one is what plays")
{
    // Alternates the note between 1.3 and 1.1, as a model rewriting the clip
    // would: notes really come and go. The last rewrite moves it to 1.3.
    const std::string at1 = "@length 1bar\n\n1.1 C3 1/8 v100\n";
    const std::string at3 = "@length 1bar\n\n1.3 C3 1/8 v100\n";
    constexpr int rewrites = 21;

    FakeAudioDevice device;
    commands::CommandBus bus (oneClip (at1));
    engine::EditSync sync (testEngine(), bus);
    REQUIRE (timedPlay (bus, lee).ok);

    // A second of rewrites from the start, then a full pass after the one they end in.
    AudioThread audio (device, 5.0);
    audio.stopAfter ((2 * barSamples + blockSize - 1) / blockSize);
    rewrite (bus, rewrites, 50ms, [&] (int i) { return i % 2 == 0 ? at3 : at1; });
    runMessagesUntil ([&audio] { return audio.isDone(); });

    CHECK (bus.history().size() == rewrites);
    REQUIRE (audio.blocksPlayed() * blockSize >= 2 * barSamples);

    const auto stressed = timing (audio.processingSeconds(), 0, audio.blocksPlayed());
    MESSAGE ("moving notes: " << describe (stressed));

#ifdef NDEBUG
    CHECK (stressed.overBudget == 0);
#endif

    // The second pass has only the last rewrite's note, at 1.3.
    const auto& out = audio.output();
    CHECK (peakBetween (out, barSamples, barSamples + 2 * beatSamples - 441) < 1.0e-3f);
    CHECK (std::abs (firstSoundAt (out, barSamples) - (barSamples + 2 * beatSamples)) < 441);
}

TEST_CASE ("stress: rewrites that change the notes leave none of them stuck")
{
    // What hocket-stress --vary does to the demo's bass line: every other
    // rewrite moves each note up an octave, so notes start and stop at all
    // points of the pass. Then the clip is emptied, and everything must fall
    // silent once the release tails are over.
    const std::string demo = "@length 2bars\n\n"
                             "1.1 C2 1/8 v100\n1.1.3 C2 1/16 v70\n1.2 C3 1/8 v90\n1.2.3 C2 1/8 v80\n"
                             "1.3 Eb2 1/8 v95\n1.3.3 G2 1/8 v85\n1.4 Bb1 1/8 v90\n1.4.3 C2 1/8 v80\n"
                             "2.1 Ab1 1/4 v100\n2.2 Ab1 1/8 v75\n2.2.3 C2 1/8 v85\n2.3 G1 1/4 v100\n2.4 G1 1/8 v80\n2.4.3 Bb1 1/8 v90\n";

    auto octaveUp = *notes::parse (demo).clip;

    for (auto& note : octaveUp.notes)
        note.pitch += 12;

    const auto up = notes::write (octaveUp);
    constexpr int rewrites = 200;

    FakeAudioDevice device;
    commands::CommandBus bus (oneClip (demo));
    engine::EditSync sync (testEngine(), bus);
    auto& midi = MidiRecorder::addTo (sync.edit());
    REQUIRE (timedPlay (bus, lee).ok);

    AudioThread audio (device, 13.0);
    std::vector<int> rewrittenAt; // the sample each rewrite was sent at, roughly
    rewrittenAt.reserve (rewrites);

    rewrite (bus, rewrites, 50ms, [&] (int i)
    {
        rewrittenAt.push_back (audio.blocksPlayed() * blockSize);
        return "# rewrite " + std::to_string (i + 1) + "\n" + (i % 2 == 1 ? up : demo);
    });

    runMessagesUntil ([until = Clock::now() + 50ms] { return Clock::now() >= until; });
    REQUIRE (bus.execute (commands::WriteClip { "clp-bass", "@length 2bars\n" }, claude).ok);
    const auto emptiedAt = audio.blocksPlayed() * blockSize;

    // 4OSC's release is 0.1 s; listen from 0.5 s after the clip was emptied.
    const auto from = emptiedAt + static_cast<int> (0.5 * FakeAudioDevice::sampleRate);
    audio.stopAfter ((from + static_cast<int> (FakeAudioDevice::sampleRate)) / blockSize);
    runMessagesUntil ([&audio] { return audio.isDone(); });

    const auto& out = audio.output();
    const auto to = audio.blocksPlayed() * blockSize;
    REQUIRE (to - from >= beatSamples);
    REQUIRE (peakBetween (out, 0, emptiedAt) > 0.01f);

    const auto peak = peakBetween (out, from, to);
    CHECK_MESSAGE (peak < 1.0e-3f, "still sounding 0.5 s after the clip was emptied, at a peak of " << peak << ": a stuck note");

    // And whose doing it would be: notes the MIDI leaves on are Tracktion's (a
    // note-off never sent; this was it, through MidiNode, before EditSync
    // turned the clips' proxies off). A sound with no note left on would be
    // the synth's (a note-off ignored).
    const auto events = midi.recorded();
    const auto left = notesLeftOn (events);

    for (const auto note : left)
    {
        const auto lastOn = std::find_if (events.rbegin(), events.rend(), [note] (const auto& e) { return e.note == note && e.on; });
        const auto sentBefore = std::ranges::upper_bound (rewrittenAt, lastOn == events.rend() ? 0 : static_cast<int> (lastOn->sample));
        MESSAGE ("left on: note " << note << ". Its last events: " << historyOf (note, events) << "The rewrite sent just before its last note-on: "
                                  << (sentBefore == rewrittenAt.begin() ? -1 : *std::prev (sentBefore)));
    }

    CHECK_MESSAGE (left.empty(), "the MIDI reaching 4OSC leaves " << left.size() << " note(s) on");
    CHECK (std::ranges::count_if (events, [] (const auto& e) { return e.note >= 0 && e.on; }) > 20);

    // The MIDI check can fail: a note-on with no note-off, for a note the clip
    // never plays, put into a copy.
    auto damaged = events;
    damaged.push_back ({ emptiedAt, 127, true });
    CHECK (notesLeftOn (damaged).size() == left.size() + 1);
}

//==============================================================================
// Structural changes at 20 a second during playback (Milestone 1's step 5).
//
// The bass holds a whole-bar C3 in each of two bars; the changes happen on
// other tracks, and every bar must sound exactly like the first: no block over
// budget, no dropped block, gap, click or retrigger. Clips with notes go on
// tracks with no instrument, so they're silent, and a MidiRecorder there
// checks that no note is left on.
namespace
{
    const std::string heldTwoBars = "@length 2bars\n\n1.1 C3 1/1 v100\n2.1 C3 1/1 v100\n";
    const std::string shortNotes = "@length 1bar\n\n1.1 E3 1/8 v100\n1.3 G3 1/8 v100\n";

    // A note on every sixteenth, for clips that only last 50 ms: some always play.
    const std::string denseNotes = [] {
        std::string text = "@length 1bar\n\n";

        for (int number = 1; number <= 4; ++number)
            for (int sixteenth = 1; sixteenth <= 4; ++sixteenth)
                text += "1." + std::to_string (number) + "." + std::to_string (sixteenth) + " E3 1/32 v100\n";

        return text;
    }();

    // trk-bass with a synth and the held notes; trk-lead and trk-pad with no instrument.
    project::Project structureProject (std::vector<TestClip> leadClips = {})
    {
        auto lead = makeTrack ("trk-lead", "Lead", std::move (leadClips));
        auto pad = makeTrack ("trk-pad", "Pad", {});
        lead.track.devices.clear();
        pad.track.devices.clear();
        return makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", 0, heldTwoBars) }), std::move (lead), std::move (pad) });
    }

    struct StressRun
    {
        juce::AudioBuffer<float> out;
        std::vector<double> processing;
        int blocks = 0;
        int firstBlock = 0; // the first block played after the changes began
        double changesTook = 0.0;
        std::vector<std::vector<MidiRecorder::Event>> midi; // what each recorder `before` gave recorded
        project::Project finalProject;                      // after the changes
        std::chrono::duration<double> longestLockHold {};    // EditSync's of JUCE's audio callback lock
    };

    // Plays a bar untouched, then sends `count` commands at 20 a second, then
    // plays to the end of the bar after the last. `before` runs once the Edit
    // exists, and gives the recorders it added, whose MIDI the run keeps;
    // `after` runs once the changes are sent.
    StressRun stressWith (project::Project project, int count, const std::function<commands::Command (int)>& change,
                          const std::function<std::vector<MidiRecorder*> (engine::EditSync&)>& before = {},
                          const std::function<void (commands::CommandBus&)>& after = {})
    {
        FakeAudioDevice device;
        commands::CommandBus bus (std::move (project));
        engine::EditSync sync (testEngine(), bus);

        const auto recorders = before ? before (sync) : std::vector<MidiRecorder*> {};
        REQUIRE (timedPlay (bus, lee).ok);

        AudioThread audio (device, 4.0 + count * 0.05 + 4.0);
        runMessagesUntil ([&audio] { return audio.blocksPlayed() * blockSize >= barSamples; });

        StressRun run;
        run.firstBlock = audio.blocksPlayed();
        const auto start = Clock::now();

        for (int i = 0; i < count; ++i)
        {
            runMessagesUntil ([due = start + i * 50ms] { return Clock::now() >= due; });
            const auto result = bus.execute (change (i), claude);
            INFO ("change " << i << ": " << result.message);
            REQUIRE (result.ok);
        }

        run.changesTook = std::chrono::duration<double> (Clock::now() - start).count();

        if (after)
            after (bus);

        audio.stopAfter ((audio.blocksPlayed() * blockSize / barSamples + 2) * barSamples / blockSize + 1);
        runMessagesUntil ([&audio] { return audio.isDone(); });

        run.out = audio.output();
        run.processing = audio.processingSeconds();
        run.blocks = audio.blocksPlayed();
        run.finalProject = bus.project();
        run.longestLockHold = sync.longestAudioLockHold();

        // The recorders go with the Edit.
        for (const auto* recorder : recorders)
            run.midi.push_back (recorder->recorded());

        return run;
    }

    // The message thread kept up, and no block took longer than it lasts.
    Timing checkTiming (const StressRun& run, int count)
    {
        CHECK_MESSAGE (run.changesTook < count * 0.05 + 0.5, "the changes took " << run.changesTook << " s: the message thread fell behind");

        const auto stressed = timing (run.processing, run.firstBlock, run.blocks);
        MESSAGE ("during the changes: " << describe (stressed));

#ifdef NDEBUG
        CHECK (stressed.overBudget == 0);
#endif

        return stressed;
    }

    // Milestone 0's checks on the held bass notes, bar by bar.
    void checkHeldNotes (const StressRun& run, int count)
    {
        checkTiming (run, count);

        const auto bars = run.blocks * blockSize / barSamples;
        REQUIRE (bars >= 4);
        REQUIRE (run.out.getRMSLevel (0, checkedFrom, window) > 0.01f);

        const auto dropped = droppedBlocks (run.out, run.blocks);
        CHECK_MESSAGE (dropped.empty(), "Tracktion played " << dropped.size() << " block(s) of silence");

        for (int checked = 1; checked < bars; ++checked)
        {
            CAPTURE (checked);
            CHECK (problemsInPass (run.out, checked, dropped).empty());
        }
    }

    void checkNothingLeftOn (const std::vector<MidiRecorder::Event>& events, const char* track)
    {
        const auto left = notesLeftOn (events);
        CHECK_MESSAGE (left.empty(), track << " is left holding " << left.size() << " note(s); the first's last events: "
                                           << (left.empty() ? std::string() : historyOf (left.front(), events)));
    }
}

TEST_CASE ("stress: clips moved 20 a second, in place and between tracks, during playback")
{
    // Round and round: to 2.1, across to the pad, to 1.1 there, back to the lead at 1.1.
    const std::array<std::pair<const char*, const char*>, 4> moves { { { "2.1", "" }, { "2.1", "trk-pad" }, { "1.1", "" }, { "1.1", "trk-lead" } } };
    const auto run = stressWith (structureProject ({ makeClip ("clp-move", 0, shortNotes) }), 100,
                                 [&moves] (int i) { return commands::MoveClip { "clp-move", moves[static_cast<std::size_t> (i % 4)].first, moves[static_cast<std::size_t> (i % 4)].second }; },
                                 [] (engine::EditSync& sync) { return std::vector { &MidiRecorder::addTo (sync.edit(), 1), &MidiRecorder::addTo (sync.edit(), 2) }; },
                                 [] (commands::CommandBus& bus) { REQUIRE (bus.execute (commands::RemoveClip { "clp-move" }, claude).ok); });

    checkHeldNotes (run, 100);
    REQUIRE (run.midi.size() == 2);
    CHECK (std::ranges::count_if (run.midi[0], [] (const auto& e) { return e.note >= 0 && e.on; }) > 10); // the clip did play
    checkNothingLeftOn (run.midi[0], "trk-lead");
    checkNothingLeftOn (run.midi[1], "trk-pad");
}

TEST_CASE ("stress: clips added and removed 20 a second during playback")
{
    const auto run = stressWith (structureProject(), 100,
                                 [] (int i) -> commands::Command
                                 {
                                     if (i % 2 == 1)
                                         return commands::RemoveClip { "clp-new" };

                                     return commands::AddClip { "trk-lead", i % 4 == 0 ? "1.1" : "2.1", commands::ClipText { denseNotes }, "clp-new", {} };
                                 },
                                 [] (engine::EditSync& sync) { return std::vector { &MidiRecorder::addTo (sync.edit(), 1) }; });

    checkHeldNotes (run, 100);
    REQUIRE (run.midi.size() == 1);
    const auto played = std::ranges::count_if (run.midi[0], [] (const auto& e) { return e.note >= 0 && e.on; });
    MESSAGE ("notes the added clips played: " << played);
    CHECK (played > 10); // they did play
    checkNothingLeftOn (run.midi[0], "trk-lead");
}

TEST_CASE ("stress: tracks with a synth added and removed 20 a second during playback")
{
    const auto run = stressWith (structureProject(), 100, [] (int i) -> commands::Command
    {
        if (i % 2 == 1)
            return commands::RemoveTrack { "trk-extra" };

        return commands::AddTrack { "midi", "Extra", "trk-extra", { "trk-lead", {} }, commands::Instrument { "hocket:synth", {} } };
    });

    checkHeldNotes (run, 100);
}

TEST_CASE ("stress: an instrument added and removed 20 a second during playback")
{
    const auto run = stressWith (structureProject(), 100, [] (int i)
    {
        return commands::SetInstrument { "trk-lead", i % 2 == 0 ? std::optional (commands::Instrument { "hocket:synth", {} }) : std::nullopt };
    });

    checkHeldNotes (run, 100);
}

TEST_CASE ("stress: a loop region played once, in real time: the note across its end ends there and rings out, none after it is struck, and the stop comes a little late")
{
    // At 240 BPM; the loop region is bar 3. The bass's clip from 3.1 has E3
    // from 3.3 across the region's end to 4.3, and G3 at 4.1; the lead has A3
    // at 4.1. With loop off, a message-thread timer stops playback once the
    // playhead passes the end, a little late. Until then the Edit's clips end
    // at the region's end (Lee's decision, 2026-11-17), so E3 ends there on
    // time, and neither G3 nor A3 is struck, however late the stop. Before
    // that, the next note was struck in every run. This also measures how
    // late the stop is, in real time; hocket-headless's message thread may be
    // slower than this test's, which asks Windows for 1 ms timers.
    constexpr double tempo = 240.0;
    constexpr int bar240 = 44100; // a bar at 240 BPM, in samples
    constexpr int e3 = 52;
    constexpr int runs = 5;

    std::vector<double> lateMs;
    std::vector<double> e3OffMs;

    for (int run = 0; run < runs; ++run)
    {
        CAPTURE (run);
        FakeAudioDevice device;
        commands::CommandBus bus (makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-across", 2 * bar, "@length 2bars\n\n1.3 E3 1/1 v100\n2.1 G3 1/8 v100\n") }),
                                                 makeTrack ("trk-lead", "Lead", { makeClip ("clp-after", 3 * bar, "@length 1bar\n\n1.1 A3 1/8 v100\n") }) },
                                               tempo));
        engine::EditSync sync (testEngine(), bus);
        auto& bass = MidiRecorder::addTo (sync.edit(), 0);
        auto& lead = MidiRecorder::addTo (sync.edit(), 1);
        REQUIRE (bus.execute (commands::SetLoopRegion { "3.1", "4.1" }, lee).ok);
        REQUIRE (bus.execute (commands::SetLooping { false }, lee).ok);
        REQUIRE (timedPlay (bus, lee).ok);

        AudioThread audio (device, 4.0);
        runMessagesUntil ([&] { return bus.isPlaying() == false || audio.blocksPlayed() * blockSize > 3 * bar240; });
        audio.stopAfter (audio.blocksPlayed() + 4);
        runMessagesUntil ([&audio] { return audio.isDone(); });

        CHECK (bus.isPlaying() == false);
        CHECK_FALSE (sync.stoppedByItself());
        checkEditMatches (sync, bus); // the clips are whole again

        // The stop ends every note at once; play did too, at the start.
        const auto events = bass.recorded();
        INFO (historyOf (e3, events));
        const auto stop = std::ranges::find_if (events, [] (const MidiRecorder::Event& e) { return e.note < 0 && e.sample > bar240 / 2; });
        REQUIRE (stop != events.end());
        lateMs.push_back (static_cast<double> (stop->sample - bar240) * 1000.0 / FakeAudioDevice::sampleRate);

        // E3 ends at the region's end, within a block, before the stop.
        const auto e3Off = std::ranges::find_if (events, [] (const MidiRecorder::Event& e) { return e.note == e3 && ! e.on; });
        REQUIRE (e3Off != events.end());
        e3OffMs.push_back (static_cast<double> (e3Off->sample - bar240) * 1000.0 / FakeAudioDevice::sampleRate);
        CHECK (std::abs (e3Off->sample - bar240) <= blockSize);

        // Nothing after the end is struck, on either track.
        const auto struckAfterEnd = [] (const MidiRecorder::Event& e) { return e.note >= 0 && e.on && e.sample >= bar240; };
        CHECK (std::ranges::none_of (events, struckAfterEnd));
        CHECK (std::ranges::none_of (lead.recorded(), [] (const MidiRecorder::Event& e) { return e.note >= 0 && e.on; }));

        // And E3 rings out after its note-off.
        CHECK (peakBetween (audio.output(), bar240, bar240 + blockSize) > 0.01f);
    }

    const auto describeMs = [] (std::vector<double> values)
    {
        std::ranges::sort (values);
        std::ostringstream text;
        text.precision (3);

        for (const auto ms : values)
            text << ms << " ms ";

        return text.str();
    };

    MESSAGE ("loop region played once: E3 ended at " << describeMs (e3OffMs) << "from the region's end; the stop came after it by " << describeMs (lateMs)
                                                       << "(a block is " << blockSeconds * 1000.0 << " ms)");

    // Late, but within a few blocks.
    CHECK (lateMs.front() >= 0.0);
    CHECK (lateMs.back() < 50.0);
}

//==============================================================================
// The mix and the tempo changed 20 a second during playback (Milestone 1's
// step 6, part 5).
//
// The bass's sound is SteadyTone's sine, whose value at every sample is known,
// so each channel's gain can be read from each sample. Tracktion's volume
// plugin takes a gain to a new value in a straight ramp over 15 ms, so a gain
// that moves faster than that is a click, or a dropped block.
namespace
{
    // The ramp: VolumeAndPanPlugin's juce::SmoothedValue, linear over 15 ms.
    const int rampSamples = static_cast<int> (std::floor (0.015 * FakeAudioDevice::sampleRate));

    project::Project toneProject (std::string_view notesText = heldTwoBars)
    {
        return makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", 0, notesText) }) });
    }

    // Puts SteadyTone on the bass, after its synth.
    void addTone (engine::EditSync& sync)
    {
        SteadyTone::addTo (*tracktion::getAudioTracks (sync.edit())[0]);
        sync.edit().getUndoManager().clearUndoHistory();
    }

    // Each channel's gain at each sample: the sample over the sine's value
    // there, or NaN where the sine is too near zero to tell.
    using GainTrace = std::array<std::vector<double>, 2>;

    GainTrace traceGains (const juce::AudioBuffer<float>& out, int samples)
    {
        const auto sine = [] (int phase)
        {
            return std::sin (juce::MathConstants<double>::twoPi * (phase % SteadyTone::period) / SteadyTone::period);
        };

        // Where the sine's periods start in the output: found from the first
        // bar, played untouched at a steady gain.
        int offset = 0;
        double best = -1.0;

        for (int candidate = 0; candidate < SteadyTone::period; ++candidate)
        {
            double sum = 0.0;

            for (int i = checkedFrom; i < checkedFrom + 4 * SteadyTone::period; ++i)
                sum += out.getSample (0, i) * sine (i + candidate);

            if (sum > best)
            {
                best = sum;
                offset = candidate;
            }
        }

        GainTrace trace;

        for (int channel = 0; channel < 2; ++channel)
        {
            auto& gains = trace[static_cast<std::size_t> (channel)];
            gains.resize (static_cast<std::size_t> (samples));

            for (int i = 0; i < samples; ++i)
            {
                const auto value = SteadyTone::level * sine (i + offset);
                gains[static_cast<std::size_t> (i)] = std::abs (value) < 0.5 * SteadyTone::level ? std::numeric_limits<double>::quiet_NaN()
                                                                                                  : out.getSample (channel, i) / value;
            }
        }

        return trace;
    }

    // The first place in [from, to) where a channel's gain moves faster than
    // the volume plugin's ramp would for a change of `largestChange` (between
    // the largest and smallest gains the test sets): a click, or a dropped
    // block. Empty if there's none.
    std::string gainJumps (const GainTrace& trace, int from, int to, double largestChange)
    {
        const auto perSample = largestChange / rampSamples * 1.05;

        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            const auto& gains = trace[channel];
            int last = -1;

            for (int i = from; i < to; ++i)
            {
                const auto gain = gains[static_cast<std::size_t> (i)];

                if (std::isnan (gain))
                    continue;

                if (last >= 0 && std::abs (gain - gains[static_cast<std::size_t> (last)]) > perSample * (i - last) + 1.0e-5)
                {
                    std::ostringstream text;
                    text << (channel == 0 ? "left" : "right") << " channel: the gain went from " << gains[static_cast<std::size_t> (last)] << " at sample "
                         << last << " to " << gain << " at sample " << i << ", faster than a 15 ms ramp";
                    return text.str();
                }

                last = i;
            }
        }

        return {};
    }

    // A channel's gain at the end of the run.
    double finalGain (const GainTrace& trace, std::size_t channel)
    {
        const auto& gains = trace[channel];
        const auto last = std::find_if (gains.rbegin(), gains.rend(), [] (double gain) { return ! std::isnan (gain); });
        return last == gains.rend() ? std::numeric_limits<double>::quiet_NaN() : *last;
    }

    std::string microseconds (std::chrono::duration<double> time)
    {
        std::ostringstream text;
        text.precision (3);
        text << time.count() * 1.0e6 << " us";
        return text.str();
    }
}

TEST_CASE ("stress: volume and pan changed 20 a second during playback, with no click")
{
    // Each change a big one: volume between +6 dB and silence, pan between
    // the hard sides, so every gain ramps all the way, often both at once.
    const std::array<const char*, 5> volumes { "+6dB", "-30dB", "0dB", "-infdB", "-6.5dB" };
    const std::array<const char*, 7> pans { "L100", "R100", "C", "L50", "R1", "L1", "R73" };
    const auto run = stressWith (toneProject(), 100, [&volumes, &pans] (int i)
    {
        return commands::SetMix { "trk-bass", volumes[static_cast<std::size_t> (i % 5)], pans[static_cast<std::size_t> (i % 7)], {}, {}, "fader-drag" };
    },
    [] (engine::EditSync& sync) { addTone (sync); return std::vector<MidiRecorder*> {}; });

    checkTiming (run, 100);
    const auto samples = run.blocks * blockSize;
    const auto trace = traceGains (run.out, samples);
    const auto jumps = gainJumps (trace, checkedFrom, samples, 2.0);
    CHECK_MESSAGE (jumps.empty(), jumps);

    // The check can fail, even allowing for ramps this big: copies with a
    // block silenced (a dropped block), and with a 2% step in one channel's
    // gain (a click), in the untouched first bar.
    const auto damagedAt = checkedFrom + 5000;
    auto dropped = run.out;
    dropped.clear (damagedAt, blockSize);
    CHECK_FALSE (gainJumps (traceGains (dropped, samples), checkedFrom, samples, 2.0).empty());
    auto stepped = run.out;
    stepped.applyGain (0, damagedAt, barSamples - damagedAt, 1.02f);
    CHECK_FALSE (gainJumps (traceGains (stepped, samples), checkedFrom, samples, 2.0).empty());

    // The last change is what plays.
    const auto expected = specifiedGains (run.finalProject, run.finalProject.tracks[0]);
    CHECK (run.finalProject.tracks[0].pan == 100); // R100, the last change's
    CHECK_MESSAGE (sameGain (finalGain (trace, 0), expected.left), "left " << finalGain (trace, 0) << ", should be " << expected.left);
    CHECK_MESSAGE (sameGain (finalGain (trace, 1), expected.right), "right " << finalGain (trace, 1) << ", should be " << expected.right);
}

TEST_CASE ("stress: mute toggled 20 a second during playback, with no click, and each toggle heard")
{
    const auto run = stressWith (toneProject(), 100, [] (int i) { return commands::SetMix { "trk-bass", {}, {}, i % 2 == 0, {}, {} }; },
                                 [] (engine::EditSync& sync) { addTone (sync); return std::vector<MidiRecorder*> {}; });

    checkTiming (run, 100);
    const auto samples = run.blocks * blockSize;
    const auto trace = traceGains (run.out, samples);
    const auto jumps = gainJumps (trace, checkedFrom, samples, 1.0);
    CHECK_MESSAGE (jumps.empty(), jumps);

    // The sound fades to silence and back each time: 50 times, unless two
    // toggles came too close together to finish a fade (the message thread
    // running late), which the timing checks above would show.
    int falls = 0;
    int rises = 0;
    bool sounding = true;

    for (const auto gain : trace[0])
    {
        if (std::isnan (gain))
            continue;

        if (sounding && gain < 1.0e-3)
        {
            ++falls;
            sounding = false;
        }
        else if (! sounding && gain > 1.0 - 1.0e-3)
        {
            ++rises;
            sounding = true;
        }
    }

    MESSAGE ("muted and unmuted in full " << falls << " and " << rises << " times, of 50");
    CHECK (falls >= 45);
    CHECK (rises == falls);
    CHECK (sounding);
    CHECK (finalGain (trace, 0) == doctest::Approx (1.0));
    CHECK (finalGain (trace, 1) == doctest::Approx (1.0));
}

TEST_CASE ("stress: tempo changed 20 a second during playback: a held note isn't struck again, no click, and the audio callback lock is held briefly")
{
    // One C3 held for 16 bars, longer than the run at any of these tempos, so
    // the song doesn't loop and the note is struck once. Each change moves
    // every clip in time, so Tracktion rebuilds its graph, and EditSync
    // holds the audio callback lock to change the tempo and the playhead
    // together. 15 more tracks of 8 clips each, with no instrument so the
    // tone is all that's heard, make it a song's worth of clips to move.
    std::vector<TestTrack> tracks;
    tracks.push_back (makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", 0, "@length 16bars\n\n1.1 C3 16/1 v100\n") }));

    for (int t = 1; t <= 15; ++t)
    {
        std::vector<TestClip> clips;

        for (int c = 0; c < 8; ++c)
            clips.push_back (makeClip ("clp-t" + std::to_string (t) + "-" + std::to_string (c), c * 2 * bar, "@length 2bars\n\n1.1 E3 1/16 v90\n1.3 G3 1/16 v90\n2.1 B3 1/8 v90\n"));

        tracks.push_back (makeTrack ("trk-t" + std::to_string (t), "Track " + std::to_string (t), std::move (clips)));
        tracks.back().track.devices.clear();
    }

    const std::array<double, 6> tempos { 140.0, 60.0, 200.0, 95.5, 300.0, 120.0 };
    const auto run = stressWith (makeProject (std::move (tracks)), 100,
                                 [&tempos] (int i) { return commands::SetTempo { tempos[static_cast<std::size_t> (i % 6)], "tempo-drag" }; },
                                 [] (engine::EditSync& sync)
                                 {
                                     auto* recorder = &MidiRecorder::addTo (sync.edit());
                                     addTone (sync);
                                     return std::vector { recorder };
                                 });

    const auto stressed = checkTiming (run, 100);
    MESSAGE ("tempo changes on 16 tracks, 121 clips: the longest hold of the audio callback lock was " << microseconds (run.longestLockHold)
                                                                                                       << "; the slowest block took " << stressed.slowest * 1000.0
                                                                                                       << " ms of its " << blockSeconds * 1000.0 << " ms");
    CHECK (run.finalProject.tempo == 95.5); // the last change's
    CHECK (run.longestLockHold > std::chrono::duration<double>::zero());

#ifdef NDEBUG
    // The audio thread waits while it's held, so even the slowest block,
    // started after the longest hold, finishes in time.
    CHECK (run.longestLockHold.count() + stressed.slowest < blockSeconds);
#endif

    // The tone's gain never moves: no dropped block, no click.
    const auto samples = run.blocks * blockSize;
    const auto trace = traceGains (run.out, samples);
    const auto jumps = gainJumps (trace, checkedFrom, samples, 0.0);
    CHECK_MESSAGE (jumps.empty(), jumps);
    CHECK (finalGain (trace, 0) == doctest::Approx (1.0));

    // C3 struck once, at the start, and never ended or struck again: no jump
    // of the playhead (which ends every note), no retrigger, no note-off.
    REQUIRE (run.midi.size() == 1);
    const auto& events = run.midi[0];
    INFO (historyOf (48, events));
    CHECK (std::ranges::count_if (events, [] (const MidiRecorder::Event& e) { return e.note >= 0 && e.on; }) == 1);
    CHECK (std::ranges::count_if (events, [] (const MidiRecorder::Event& e) { return e.note >= 0 && ! e.on; }) == 0);
    CHECK (std::ranges::count_if (events, [] (const MidiRecorder::Event& e) { return e.note < 0 && e.sample > 0; }) == 0);
}
