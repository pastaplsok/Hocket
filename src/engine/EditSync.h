#pragma once

// The sync layer (SPEC §3.3): builds a Tracktion Edit from the command bus's
// project, then keeps it in step as commands apply. Hocket's model is the
// source of truth; the Edit is derived state, never read back or saved.
//
// Tracks, devices and clips are matched by their Hocket IDs. Each change the
// bus announces names a track, clip or file; that one is brought in line with
// the model as it is now, whatever the change was, so the order of a
// command's changes doesn't matter. Every track's mix is checked after every
// change, since one track's solo silences the others. A tempo change during
// playback keeps the playhead's place in the bar, and the notes sounding.
//
// It's also the bus's transport (SPEC §3.1), following the bus's transport
// settings: play plays the loop region or the whole song, on a loop or once.
// While a loop region plays once, the Edit's clips end at the region's end
// (clipsEndBy), so nothing after it is heard; otherwise the Edit is the model.
//
// During playback, a command's changes reach the Edit together, and only once
// the graph Tracktion last built has played a block; changes that come before
// then wait, and go in together (SPEC §3.3). Tracktion builds each graph
// against the last one it built, played or not, and what a graph is to do
// once it does when it first plays: a clip's player ends the notes a rewrite
// took away, and a track's ends those of a clip taken out. Built over one
// that never played, those note-offs were lost, and the notes stayed on
// (several requests in one audio block). So every graph EditSync has Tracktion
// build during playback waits for the last to play, but for a jump's (a seek,
// a play, a wrap), and those after one before a block has played: the jump's
// block ends every note the graph has sounding, so nothing is lost, and what's
// waiting goes in with the jump. EditSync builds the graph itself (build), at
// once, after each change it makes to the Edit, so Tracktion's own timer never
// builds over one.
//
// A note sounds only when the playhead reaches its start (SPEC §3.3), but
// Tracktion's players strike the notes a clip has sounding where the playhead
// is when the clip's player is new (a clip added or remade) or the playhead
// jumps. So those notes are left out of the Edit (heldNotes) until a block
// has played without them, then put back (settle): notes added to a clip the
// graph already has wait for their starts. For a clip made during playback,
// that's its notes sounding anywhere the playhead may be before the graph
// has the clip; for a wrap (updateLoop), those where it lands.
//
// A clip taken out during playback is emptied first, so its own player ends
// the notes it has sounding, and leaves the Edit once a block has played
// (endingClips), so the graph without it has nothing left to do.
//
// Message thread only, like the bus.

#include "core/commands/CommandBus.h"
#include "engine/devices/DrumSamplerPlugin.h"
#include "engine/kits/KitLibrary.h"
#include "engine/mix/GainStagePlugin.h"

#include <tracktion_engine/tracktion_engine.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hocket::engine
{
    class EditSync final : private commands::Transport,
                           private juce::Timer
    {
    public:
        // Builds the Edit, then registers with the bus as a listener and as its
        // transport. The bus must outlive this. Drum samplers load their kits
        // from `kits`, which must outlive this too; with none, they're silent.
        // Kits load on the library's thread: wait for them (KitLibrary::
        // waitUntilIdle) before playing or rendering.
        EditSync (tracktion::Engine& engine, commands::CommandBus& bus, KitLibrary* kits = nullptr);
        ~EditSync() override;

        EditSync (const EditSync&) = delete;
        EditSync& operator= (const EditSync&) = delete;

        tracktion::Edit& edit() { return *tracktionEdit; }

        // What play plays, on a loop or once (commands::playRange): the loop
        // region, or from the song's start to the end of its last clip,
        // rounded up to a bar line, and at least one bar.
        tracktion::TimeRange loopRange() const;

        // Whether playback has stopped though the last transport command was
        // play. Tracktion stops by itself when it rebuilds its device lists: a
        // MIDI or audio device plugged in, unplugged, or reset by its driver.
        bool stoppedByItself() const;

        // The longest this has held JUCE's audio callback lock, during which
        // no audio block can start (CLAUDE.md): for the stress tests.
        std::chrono::steady_clock::duration longestAudioLockHold() const { return longestLockHold; }

        // A note of a clip: its start in the clip, and its pitch, which
        // together say which (SPEC §5.4).
        struct HeldNote
        {
            notes::Ticks start = 0;
            int pitch = 0;

            friend bool operator== (const HeldNote&, const HeldNote&) = default;
        };

        // The notes left out of the Edit's clips, by clip ID, until a block
        // has played without them, and the stretches of the song where the
        // playhead was, or would be, when they were: each held note sounds
        // somewhere in one of them (its start before the stretch's end, its
        // end after the stretch's start). For tests (checkEditMatches).
        const std::map<std::string, std::vector<HeldNote>, std::less<>>& heldNotes() const { return held; }
        const std::vector<commands::TickRange>& heldWhere() const { return heldStretches; }

        // Clips taken out during playback, emptied, until a block has played
        // them so: each is named after the clip, with " (ending)". For tests.
        const std::vector<tracktion::MidiClip::Ptr>& endingClips() const { return ending; }

        // Whether changes the bus has announced are waiting for the graph
        // Tracktion last built to play a block. For tests.
        bool changesWaiting() const { return waiting.any(); }

    private:
        // A Hocket track in the Edit, the plugins its devices made, by device
        // ID, and the gain stage that plays its mix, after them.
        struct SyncedTrack
        {
            tracktion::AudioTrack::Ptr track;
            std::map<std::string, tracktion::Plugin::Ptr, std::less<>> plugins;
            juce::ReferenceCountedObjectPtr<GainStagePlugin> gainStage;
        };

        commands::CommandBus& bus;
        KitLibrary* kitLibrary = nullptr;
        std::unique_ptr<tracktion::Edit> tracktionEdit;
        std::map<std::string, SyncedTrack, std::less<>> tracks;             // by Hocket track ID
        std::map<std::string, tracktion::MidiClip::Ptr, std::less<>> clips; // by Hocket clip ID
        int listenerId = 0;
        bool playRequested = false; // by the last transport command
        std::chrono::steady_clock::duration longestLockHold {};

        // JUCE's audio callback lock, held for a scope, and the hold timed.
        class AudioLock;

        // While a loop region plays once (loop off), its end, where the
        // Edit's clips end: the notes sounding there end on time and ring
        // out, and nothing after it starts before timerCallback's stop, which
        // comes a little late. A clip that starts at or after it isn't in the
        // Edit. None otherwise, and then the Edit's clips are the model's.
        std::optional<notes::Ticks> clipsEndBy;

        // How much has played since play last started (playProgress). The
        // playhead's unlooped position counts every sample since it was last
        // placed, whatever the loop, and a song has one tempo, so what has
        // played since a baseline is exact. The baseline is taken at play,
        // and again wherever this moves the playhead (a seek, a wrap), with
        // what had played up to then added in. Tracktion places it itself
        // after a new tempo map; the 50 ms sampling covers that.
        struct Progress
        {
            bool started = false;
            bool running = false;
            std::chrono::steady_clock::time_point startedAt {};
            std::chrono::steady_clock::time_point lastSampledAt {};
            notes::Ticks ticks = 0;          // up to the baseline
            std::int64_t baseUnlooped = 0;   // the playhead's unlooped position at the baseline, in samples
            std::int64_t basePlayout = 0;    // its playout sync position then: another means it was placed since
            notes::Ticks basePlace = 0;      // its place in the song then
        };

        class ProgressTimer final : public juce::Timer
        {
        public:
            explicit ProgressTimer (EditSync& owner) : sync (owner) {}
            void timerCallback() override { sync.sampleProgress(); }

        private:
            EditSync& sync;
        };

        Progress progress;
        ProgressTimer progressTimer { *this };
        std::optional<commands::PlayheadWrap> lastWrap; // for takeWrap

        // Notes left out of the Edit (holdNotesSounding), and clips taken out
        // during playback, emptied (removeClip), until a block has played
        // since: settle puts them back and takes them out.
        std::map<std::string, std::vector<HeldNote>, std::less<>> held;
        std::vector<commands::TickRange> heldStretches;
        std::vector<tracktion::MidiClip::Ptr> ending;

        // What the bus has changed that the Edit doesn't have yet (changed),
        // by what to bring in line with the model: brought in by catchUp.
        struct Waiting
        {
            std::set<std::string, std::less<>> tracks, clips, files;
            bool tempo = false;
            bool clipsEnd = false; // clipsEndBy is to follow the transport settings

            bool any() const { return tempo || clipsEnd || ! tracks.empty() || ! clips.empty() || ! files.empty(); }
        };

        Waiting waiting;

        // The gate: whether the graph Tracktion last built during playback
        // may not have played a block yet, from which block it has once that
        // one has started (a sample of the playhead's reference count), and
        // the count when it was built. And whether the playhead has jumped
        // since: then another graph may be built over it.
        bool builtUnplayed = false;
        juce::Range<std::int64_t> builtDuring;
        std::int64_t playedFrom = 0;
        bool jumpedSince = false;

        class GateTimer final : public juce::Timer
        {
        public:
            explicit GateTimer (EditSync& owner) : sync (owner) {}
            void timerCallback() override { sync.catchUp(); }

        private:
            EditSync& sync;
        };

        GateTimer gateTimer { *this };

        using HeldByClip = std::map<std::string, std::vector<HeldNote>, std::less<>>;

        // Adds to `held`, and to `taken`, the notes in the Edit's clips (or
        // only `onlyClip`) sounding anywhere in `where` (their start before
        // its end, their end after its start). The caller rewrites the clips.
        void holdNotesSounding (commands::TickRange where, HeldByClip& taken, const std::string* onlyClip = nullptr);

        // The stretches of the song the playhead may play in the next moment,
        // within its loop: where a clip made now may first be played.
        std::vector<commands::TickRange> placesAhead (tracktion::graph::PlayHead&, double sampleRate) const;

        // Puts these held notes back in the Edit. The caller builds.
        void releaseHeldNotes (const HeldByClip& notes);

        // After a command's changes, the transport's, and on a timer while
        // anything waits: brings what's waiting into the Edit, and settles,
        // when the gate allows, and builds. Stopped, it always does.
        void catchUp();

        // Brings what's waiting into the Edit; the tempo only with `tempo`.
        // The caller builds.
        void bringWaitingIn (bool tempo = true);

        // Puts all held notes back and takes the emptied clips out. Only once
        // a block has played since they were held or emptied (or stopped),
        // and the caller builds.
        void settle();

        // Stopped: brings everything in, settles, and builds.
        void settleStopped();

        // Has Tracktion build the graph now, from the changes made to the
        // Edit, and notes when for the gate. During playback, only when the
        // gate allows (mayBuild) or before a jump.
        void build();

        // Whether the graph Tracktion last built has played a block, or isn't
        // playing; and whether another may be built over it now: also once
        // the playhead has jumped since it was built.
        bool lastBuildHasPlayed() const;
        bool mayBuild() const { return jumpedSince || lastBuildHasPlayed(); }

        // After a jump this made, while the audio callback lock was held and
        // `next` the playhead's reference range: settling waits for the
        // jump's block to have played.
        void noteJump (juce::Range<std::int64_t> next);

        // The device's block size, in samples.
        int blockSize() const;

        // This clip's held notes, or nullptr.
        const std::vector<HeldNote>* heldIn (const std::string& clipId) const;

        void sampleProgress();           // adds what has played to progress.ticks, and takes a new baseline
        void takeProgressBaseline();
        notes::Ticks ticksPlayed() const; // progress.ticks and what has played since the baseline
        notes::Ticks ticksAt (std::int64_t samplePosition, double sampleRate) const;

        // Notes a change for catchUp, and changesDone takes a command's
        // changes together.
        void changed (const commands::Change&);
        void changesDone();

        // Each brings one thing in the Edit in line with the model: makes it,
        // changes it, or removes it.
        void syncTrack (const std::string& trackId);
        void syncClip (const std::string& clipId);
        void syncFile (const std::string& file);

        void syncDevices (const project::Track&, SyncedTrack&);
        void syncTrackOrder();
        void removeTrack (const std::string& trackId);
        void removeClip (const std::string& clipId);

        // Every track's volume, pan, mute and solo, as both channels' gains,
        // through its gain stage, which ramps changes so they don't click.
        void syncMix();

        // The tempo, with every clip moved to keep its beats, and the
        // playhead's place and loop changed to match in the same instant.
        void syncTempo();

        // The Tracktion track a Hocket track at this place in the display order goes after.
        tracktion::Track* trackBefore (std::size_t index) const;

        // What a playhead outside the loop does: wraps into it, or, with loop
        // off, plays on (and timerCallback stops it at the end).
        enum class Outside
        {
            wrap,             // a new loop region, or looping on or off (SPEC §3.1)
            wrapWhileLooping, // the song's end moved: played once, it has ended
        };

        // Sets the loop to loopRange(), and looping on or off as the bus's
        // transport settings say, and hands both to the playhead at once.
        void updateLoop (Outside);

        // Where the Edit's clips end while playing: the loop region's end
        // when loop is off. None while looping, or with no region, since
        // nothing follows the song's own end.
        std::optional<notes::Ticks> playOnceEnd() const;

        // Cuts the Edit's clips short at `end`, leaving out those that start
        // at or after it, or with none gives them back their own ends. Only
        // clips that reach past either end are synced. The caller builds, at
        // once: play starts with the graph Tracktion has, and while playing,
        // the playhead mustn't reach the end first.
        void setClipsEnd (std::optional<notes::Ticks> end);

        // The playhead and the sample rate it counts in; none before the
        // first play.
        std::pair<tracktion::graph::PlayHead*, double> currentPlayHead() const;

        tracktion::TimePosition timeOf (notes::Ticks) const;
        tracktion::TimeRange timeOf (commands::TickRange) const;

        // JUCE's lock around each audio block. Held on the message thread,
        // through AudioLock only and as briefly as can be (microseconds;
        // a tempo change, tens of them), so a change to the playhead and what
        // it reads happens between blocks.
        juce::CriticalSection& audioCallbackLock() const;

        void clearTracktionUndo();

        void play() override;
        void stop() override;
        bool isPlaying() const override;
        notes::Ticks playheadPosition() const override;
        void jumpTo (notes::Ticks position) override;
        void loopChanged() override;
        commands::PlayProgress playProgress() const override;
        std::optional<commands::PlayheadWrap> takeWrap() override;

        // With loop off, stops playback once the playhead reaches the end,
        // and gives the clips their ends back after any stop.
        void timerCallback() override;
    };
}
