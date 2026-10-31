// The Edit built from the project, kept in step with commands, and the
// transport commands it carries out.

#include "EngineTesting.h"

#include <doctest/doctest.h>

using namespace hocket;
using namespace hocket::testing;

namespace
{
    // Two tracks: bass with clips at 1.1 (2 bars) and 5.1, lead with one at 2.3.
    project::Project twoTracks()
    {
        return makeProject ({
            makeTrack ("trk-bass", "Bass", {
                makeClip ("clp-bass-a", 0, "@length 2bars\n\n1.1 C2 1/4 v100\n1.2.3 Eb2 1/8 v90\n"),
                makeClip ("clp-bass-b", 4 * bar, "@length 1bar\n\n1.1 G1 1/2 v80\n"),
            }),
            makeTrack ("trk-lead", "Lead", {
                makeClip ("clp-lead", bar + 2 * beat, "@length 1bar\n\n1.1 [C4 E4 G4] 1/4 v70\n"),
            }),
        }, 90.0);
    }

    tracktion::MidiClip& clipNamed (tracktion::Edit& edit, const juce::String& name)
    {
        for (auto* track : tracktion::getAudioTracks (edit))
            for (auto* clip : track->getClips())
                if (clip->getName() == name)
                    if (auto* midi = dynamic_cast<tracktion::MidiClip*> (clip))
                        return *midi;

        FAIL ("no MIDI clip " << name);
        throw;
    }

    double secondsAt (tracktion::Edit& edit, double beats)
    {
        return edit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (beats)).inSeconds();
    }
}

TEST_CASE ("the Edit is built from the project: tempo, tracks, a 4OSC each, clips and notes")
{
    commands::CommandBus bus (twoTracks());
    engine::EditSync sync (testEngine(), bus);
    auto& edit = sync.edit();

    CHECK (edit.tempoSequence.getTempo (0)->getBpm() == 90.0);
    CHECK (edit.tempoSequence.getTimeSig (0)->numerator == 4);
    CHECK (edit.tempoSequence.getTimeSig (0)->denominator == 4);

    const auto tracks = tracktion::getAudioTracks (edit);
    REQUIRE (tracks.size() == 2);
    CHECK (tracks[0]->getName() == "Bass");
    CHECK (tracks[1]->getName() == "Lead");

    for (auto* track : tracks)
    {
        CHECK (dynamic_cast<tracktion::FourOscPlugin*> (track->pluginList[0]) != nullptr);
        CHECK (track->pluginList.getPluginsOfType<tracktion::FourOscPlugin>().size() == 1);
    }

    CHECK (tracks[0]->getClips().size() == 2);
    CHECK (tracks[1]->getClips().size() == 1);

    // Positions: ticks from the song start, at 90 BPM.
    auto& lead = clipNamed (edit, "clp-lead");
    CHECK (lead.getPosition().getStart().inSeconds() == doctest::Approx (secondsAt (edit, 6.0)));
    CHECK (lead.getPosition().getLength().inSeconds() == doctest::Approx (secondsAt (edit, 4.0)));
    CHECK (lead.getSequence().getNumNotes() == 3); // a chord is one note per pitch

    auto& bass = clipNamed (edit, "clp-bass-a");
    CHECK (bass.getPosition().getLength().inSeconds() == doctest::Approx (secondsAt (edit, 8.0)));
    REQUIRE (bass.getSequence().getNumNotes() == 2);

    const auto* second = bass.getSequence().getNotes()[1];
    CHECK (second->getNoteNumber() == 39); // Eb2
    CHECK (second->getVelocity() == 90);
    CHECK (second->getStartBeat().inBeats() == doctest::Approx (1.5)); // 1.2.3: beat 2, third sixteenth
    CHECK (second->getLengthBeats().inBeats() == doctest::Approx (0.5));

    // The loop runs to the end of the last clip, clp-bass-b at 5.1, so to 6.1.
    CHECK (sync.loopRange().getStart().inSeconds() == 0.0);
    CHECK (sync.loopRange().getEnd().inSeconds() == doctest::Approx (secondsAt (edit, 20.0)));

    // Undo is Hocket's job: Tracktion's own history stays empty.
    CHECK_FALSE (edit.getUndoManager().canUndo());
}

TEST_CASE ("write_clip, undo and redo update the Tracktion clip, and nothing else")
{
    commands::CommandBus bus (twoTracks());
    engine::EditSync sync (testEngine(), bus);
    auto& edit = sync.edit();
    auto& bass = clipNamed (edit, "clp-bass-a");
    auto& lead = clipNamed (edit, "clp-lead");

    REQUIRE (bus.execute (commands::WriteClip { "clp-lead", "@length 1bar\n\n1.3 A4 1/8 v60\n" }, testing::claude).ok);

    REQUIRE (lead.getSequence().getNumNotes() == 1);
    CHECK (lead.getSequence().getNotes()[0]->getNoteNumber() == 69);
    CHECK (lead.getSequence().getNotes()[0]->getStartBeat().inBeats() == doctest::Approx (2.0));
    CHECK (bass.getSequence().getNumNotes() == 2);

    REQUIRE (bus.undo (testing::claude, commands::UndoScope::callersOwn).ok);
    CHECK (lead.getSequence().getNumNotes() == 3);

    REQUIRE (bus.redo (testing::claude).ok);
    CHECK (lead.getSequence().getNumNotes() == 1);

    CHECK_FALSE (edit.getUndoManager().canUndo());
}

TEST_CASE ("a new @length changes the clip's length and the loop")
{
    commands::CommandBus bus (makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", 0, "@length 1bar\n\n1.1 C2 1/4 v100\n") }) }));
    engine::EditSync sync (testEngine(), bus);
    auto& edit = sync.edit();

    CHECK (sync.loopRange().getEnd().inSeconds() == doctest::Approx (2.0));

    REQUIRE (bus.execute (commands::WriteClip { "clp-bass", "@length 3bars\n\n1.1 C2 1/4 v100\n" }, testing::claude).ok);

    CHECK (clipNamed (edit, "clp-bass").getPosition().getLength().inSeconds() == doctest::Approx (6.0));
    CHECK (sync.loopRange().getEnd().inSeconds() == doctest::Approx (6.0));
    CHECK (edit.getTransport().getLoopRange().getEnd().inSeconds() == doctest::Approx (6.0));
}

TEST_CASE ("the loop ends on a bar line, and is at least one bar")
{
    // A 1-bar clip at 2.3 ends at 3.3, so the loop runs to 4.1.
    commands::CommandBus offBeat (makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", bar + 2 * beat, "@length 1bar\n\n1.1 C2 1/4 v100\n") }) }));
    engine::EditSync offBeatSync (testEngine(), offBeat);
    CHECK (offBeatSync.loopRange().getEnd().inSeconds() == doctest::Approx (6.0));
}

TEST_CASE ("an empty project loops one bar")
{
    commands::CommandBus bus (makeProject ({ makeTrack ("trk-bass", "Bass", {}) }));
    engine::EditSync sync (testEngine(), bus);
    CHECK (sync.loopRange().getEnd().inSeconds() == doctest::Approx (2.0));
}

TEST_CASE ("play and stop go through the bus to the Edit's transport")
{
    commands::CommandBus bus (twoTracks());

    {
        engine::EditSync sync (testEngine(), bus);
        auto& tracktionTransport = sync.edit().getTransport();

        const auto played = bus.execute (commands::Play {}, testing::claude);
        CHECK (played.ok);
        CHECK (played.message == "playing 1.1 to 6.1 on a loop");
        CHECK (tracktionTransport.isPlaying());
        CHECK (bus.isPlaying() == true);
        CHECK (tracktionTransport.looping.get());
        CHECK (tracktionTransport.getLoopRange() == sync.loopRange());

        CHECK (bus.execute (commands::Play {}, testing::lee).message == "already playing");

        CHECK (bus.execute (commands::Stop {}, testing::lee).message == "stopped");
        CHECK_FALSE (tracktionTransport.isPlaying());
        CHECK (bus.isPlaying() == false);
        CHECK (bus.execute (commands::Stop {}, testing::lee).message == "already stopped");

        CHECK (bus.history().empty());
    }

    // Once the EditSync is gone, the bus has no transport again.
    CHECK_FALSE (bus.execute (commands::Play {}, testing::claude).ok);
}
