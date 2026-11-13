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

TEST_CASE ("in 6/8, Tracktion's beats are eighths and its BPM counts quarter notes, as Hocket's do")
{
    // At 120 BPM, a 6/8 bar is six eighths of 0.25 s.
    const notes::Meter sixEight { 6, 8 };
    commands::CommandBus bus (makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", sixEight.ticksPerBar(), "@length 2bars\n\n1.4 C2 1/8 v100\n2.6 G2 1/4 v90\n") }) },
                                          120.0, sixEight));
    engine::EditSync sync (testEngine(), bus);
    auto& edit = sync.edit();

    CHECK (edit.tempoSequence.getTimeSig (0)->numerator == 6);
    CHECK (edit.tempoSequence.getTimeSig (0)->denominator == 8);
    CHECK (edit.tempoSequence.getTempo (0)->getBpm() == 120.0);

    auto& clip = clipNamed (edit, "clp-bass");
    CHECK (clip.getPosition().getStart().inSeconds() == doctest::Approx (1.5));
    CHECK (clip.getPosition().getLength().inSeconds() == doctest::Approx (3.0));

    REQUIRE (clip.getSequence().getNumNotes() == 2);
    const auto* first = clip.getSequence().getNotes()[0];
    const auto* second = clip.getSequence().getNotes()[1];
    CHECK (first->getStartBeat().inBeats() == doctest::Approx (3.0));   // 1.4: three eighths in
    CHECK (first->getLengthBeats().inBeats() == doctest::Approx (1.0)); // an eighth
    CHECK (second->getStartBeat().inBeats() == doctest::Approx (11.0)); // 2.6
    CHECK (second->getLengthBeats().inBeats() == doctest::Approx (2.0)); // a quarter is two eighths

    // The clip ends at 4.1, 4.5 s in.
    CHECK (sync.loopRange().getEnd().inSeconds() == doctest::Approx (4.5));
}

TEST_CASE ("linked clips play their file's notes, and a write to one changes them all")
{
    const auto riff = makeClip ("clp-riff", 0, "@length 1bar\n\n1.1 C3 1/4 v100\n");

    commands::CommandBus bus (makeProject ({
        makeTrack ("trk-bass", "Bass", { riff, linkedClip ("clp-riff-again", 2 * bar, riff) }),
        makeTrack ("trk-lead", "Lead", { linkedClip ("clp-riff-lead", bar, riff), makeClip ("clp-lead", 3 * bar, "@length 1bar\n") }),
    }));
    engine::EditSync sync (testEngine(), bus);
    auto& edit = sync.edit();

    for (const auto* name : { "clp-riff", "clp-riff-again", "clp-riff-lead" })
    {
        INFO (name);
        REQUIRE (clipNamed (edit, name).getSequence().getNumNotes() == 1);
        CHECK (clipNamed (edit, name).getSequence().getNotes()[0]->getNoteNumber() == 48);
    }

    REQUIRE (bus.execute (commands::WriteClip { "clp-riff-again", "@length 2bars\n\n1.1 E3 1/4 v100\n2.1 G3 1/4 v100\n" }, testing::claude).ok);

    for (const auto* name : { "clp-riff", "clp-riff-again", "clp-riff-lead" })
    {
        INFO (name);
        auto& clip = clipNamed (edit, name);
        REQUIRE (clip.getSequence().getNumNotes() == 2);
        CHECK (clip.getSequence().getNotes()[0]->getNoteNumber() == 52);
        CHECK (clip.getPosition().getLength().inSeconds() == doctest::Approx (4.0));
    }

    CHECK (clipNamed (edit, "clp-lead").getSequence().getNumNotes() == 0);
    CHECK (clipNamed (edit, "clp-riff-lead").getPosition().getStart().inSeconds() == doctest::Approx (2.0));

    // The loop runs to the end of the last clip: clp-riff-again, now 3.1 to 5.1.
    CHECK (sync.loopRange().getEnd().inSeconds() == doctest::Approx (8.0));

    REQUIRE (bus.undo (testing::claude, commands::UndoScope::callersOwn).ok);

    for (const auto* name : { "clp-riff", "clp-riff-again", "clp-riff-lead" })
        CHECK (clipNamed (edit, name).getSequence().getNumNotes() == 1);
}

TEST_CASE ("a track with no device gets no instrument")
{
    auto project = makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", 0, "@length 1bar\n\n1.1 C2 1/4 v100\n") }) });
    project.tracks[0].devices.clear();
    commands::CommandBus bus (std::move (project));
    engine::EditSync sync (testEngine(), bus);

    const auto tracks = tracktion::getAudioTracks (sync.edit());
    REQUIRE (tracks.size() == 1);
    CHECK (tracks[0]->pluginList.getPluginsOfType<tracktion::FourOscPlugin>().isEmpty());
    CHECK (tracks[0]->getClips().size() == 1);
}

TEST_CASE ("a drums track's clips play their notes, with no instrument until Hocket's drum sampler is built")
{
    auto project = makeProject ({
        makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", 0, "@length 1bar\n\n1.1 C2 1/4 v100\n") }),
        makeTrack ("trk-drums", "Drums", { makeClip ("clp-beat", 0, "@length 1bar\n\n1.1 [C2 F#2] 1/16 v100\n1.2 D2 1/16 v90\n") }),
    });

    auto& drums = project.tracks[1];
    drums.type = project::TrackType::drums;
    drums.drumMap = notes::standardDrumMap();
    drums.devices = { { "dev-drums-kit", project::DeviceType::drumSampler, "acoustic" } };

    commands::CommandBus bus (std::move (project));
    engine::EditSync sync (testEngine(), bus);

    const auto tracks = tracktion::getAudioTracks (sync.edit());
    REQUIRE (tracks.size() == 2);
    CHECK (tracks[0]->pluginList.getPluginsOfType<tracktion::FourOscPlugin>().size() == 1);
    CHECK (tracks[1]->pluginList.getPluginsOfType<tracktion::FourOscPlugin>().isEmpty());

    // The notes are pitches, whatever the clip calls them.
    REQUIRE (bus.execute (commands::WriteClip { "clp-beat", "1.1 [kick hat-closed] 1/16 v100\n1.3 snare 1/16 v90\n" }, claude).ok);
    std::vector<int> pitches;

    for (auto* note : clipNamed (sync.edit(), "clp-beat").getSequence().getNotes())
        pitches.push_back (note->getNoteNumber());

    std::ranges::sort (pitches);
    CHECK (pitches == std::vector<int> { 36, 38, 42 });
}

TEST_CASE ("play and stop go through the bus to the Edit's transport")
{
    commands::CommandBus bus (twoTracks());

    {
        engine::EditSync sync (testEngine(), bus);
        auto& tracktionTransport = sync.edit().getTransport();

        const auto played = timedPlay (bus, testing::claude);
        CHECK (played.ok);
        CHECK (played.message == "playing 1.1 to 6.1 on a loop");
        CHECK (tracktionTransport.isPlaying());
        CHECK (bus.isPlaying() == true);
        CHECK (tracktionTransport.looping.get());
        CHECK (tracktionTransport.getLoopRange() == sync.loopRange());

        CHECK (timedPlay (bus, testing::lee).message == "already playing");

        CHECK (bus.execute (commands::Stop {}, testing::lee).message == "stopped");
        CHECK_FALSE (tracktionTransport.isPlaying());
        CHECK (bus.isPlaying() == false);
        CHECK (bus.execute (commands::Stop {}, testing::lee).message == "already stopped");

        CHECK (bus.history().empty());
    }

    // Once the EditSync is gone, the bus has no transport again.
    CHECK_FALSE (timedPlay (bus, testing::claude).ok);
}

TEST_CASE ("a stop no transport command asked for is told apart from one that was")
{
    commands::CommandBus bus (twoTracks());
    engine::EditSync sync (testEngine(), bus);
    CHECK_FALSE (sync.stoppedByItself()); // it never played

    REQUIRE (timedPlay (bus, testing::lee).ok);
    CHECK_FALSE (sync.stoppedByItself());

    // As when Tracktion rebuilds its device lists.
    sync.edit().getTransport().stop (false, false);
    CHECK (sync.stoppedByItself());
    CHECK (bus.isPlaying() == false);

    REQUIRE (timedPlay (bus, testing::claude).ok);
    CHECK_FALSE (sync.stoppedByItself());

    REQUIRE (bus.execute (commands::Stop {}, testing::lee).ok);
    CHECK_FALSE (sync.stoppedByItself());
}
