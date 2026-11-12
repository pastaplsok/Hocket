// Random sequences of every command, invalid ones included (step 4's part 8).
// A refused command changes nothing; after any other, the project writes and
// reads back unchanged, so the bus never makes a project the reader would
// refuse. Undoing everything gives the original, and redoing everything the
// same end state.

#include "BusTesting.h"

#include <array>
#include <map>
#include <random>
#include <set>

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;

namespace
{
    using Rng = std::mt19937;

    int randomInt (Rng& rng, int low, int high)
    {
        return std::uniform_int_distribution<int> (low, high) (rng);
    }

    bool chance (Rng& rng, int percent)
    {
        return randomInt (rng, 1, 100) <= percent;
    }

    template <typename T, std::size_t size>
    const T& pick (Rng& rng, const std::array<T, size>& items)
    {
        return items[static_cast<std::size_t> (randomInt (rng, 0, static_cast<int> (size) - 1))];
    }

    const Author lee { "Lee" };
    const Author claude { "Claude", "session-1" };

    // A bass with two clips, a lead linked to one of them, and drums.
    project::Project makeProject()
    {
        project::Project project;
        project::Track bass { "trk-bass", "Bass", project::TrackType::midi, { { "dev-bass-synth", project::DeviceType::synth } }, {} };
        project::Track lead { "trk-lead", "Lead", project::TrackType::midi, {}, {} };
        project::Track drums { "trk-drums", "Drums", project::TrackType::drums, { { "dev-drums-kit", project::DeviceType::drumSampler, "acoustic" } }, {} };
        drums.drumMap = notes::standardDrumMap();

        project.notesFiles["clips/a.notes"] = parsedNotes ("@length 2bars\n\n1.1 C2 1/4 v100\n2.3 G2 1/8 v90\n");
        project.notesFiles["clips/b.notes"] = parsedNotes ("@length 1bar\n\n1.1 G1 1/4 v100\n");
        project.notesFiles["clips/beat.notes"] = parsedNotes ("@length 1bar\n\n1.1 kick 1/16 v110\n1.2 snare 1/16 v90\n", {}, &drums.drumMap);
        bass.clips.push_back ({ "clp-a", "clips/a.notes", 0 });
        bass.clips.push_back ({ "clp-b", "clips/b.notes", 4 * 3840 });
        lead.clips.push_back ({ "clp-a2", "clips/a.notes", 8 * 3840 });
        drums.clips.push_back ({ "clp-beat", "clips/beat.notes", 0 });

        project.tracks.push_back (std::move (bass));
        project.tracks.push_back (std::move (lead));
        project.tracks.push_back (std::move (drums));
        return project;
    }

    //==============================================================================
    // IDs, mostly real ones, sometimes not.
    std::string trackId (Rng& rng, const project::Project& project)
    {
        if (project.tracks.empty() || chance (rng, 10))
            return pick (rng, std::array<std::string, 3> { "trk-nope", "clp-a", "" });

        return project.tracks[static_cast<std::size_t> (randomInt (rng, 0, static_cast<int> (project.tracks.size()) - 1))].id;
    }

    std::string clipId (Rng& rng, const project::Project& project)
    {
        std::vector<std::string> ids;

        for (const auto& track : project.tracks)
            for (const auto& clip : track.clips)
                ids.push_back (clip.id);

        if (ids.empty() || chance (rng, 10))
            return pick (rng, std::array<std::string, 3> { "clp-nope", "trk-bass", "" });

        return ids[static_cast<std::size_t> (randomInt (rng, 0, static_cast<int> (ids.size()) - 1))];
    }

    // Bar lines and beats, mostly early in the song, with invalid ones and the song's end.
    std::string position (Rng& rng)
    {
        if (chance (rng, 5))
            return pick (rng, std::array<std::string, 4> { "1.5", "0.1", "998.1", "999.1" });

        return std::to_string (randomInt (rng, 1, 16)) + (chance (rng, 70) ? ".1" : ".2");
    }

    std::optional<std::string> maybe (Rng& rng, int percent, std::string value)
    {
        return chance (rng, percent) ? std::optional<std::string> (std::move (value)) : std::nullopt;
    }

    std::string gesture (Rng& rng)
    {
        return chance (rng, 50) ? pick (rng, std::array<std::string, 2> { "drag-1", "drag-2" }) : std::string();
    }

    std::optional<Instrument> instrument (Rng& rng)
    {
        switch (randomInt (rng, 0, 4))
        {
            case 0: return {};
            case 1: return Instrument { "hocket:synth", {} };
            case 2: return Instrument { "hocket:drum-sampler", pick (rng, std::array<std::string, 2> { "acoustic", "electronic" }) };
            case 3: return Instrument { "hocket:drum-sampler", {} };
            default: return Instrument { "vst3:serum", {} };
        }
    }

    std::vector<project::DrumMapEntry> drumMap (Rng& rng)
    {
        switch (randomInt (rng, 0, 4))
        {
            case 0:
            {
                std::vector<project::DrumMapEntry> entries;

                for (const auto& [note, name] : notes::standardDrumMap().byNote())
                    entries.push_back ({ name, note });

                return entries;
            }

            case 1: return { { "kick", 36 }, { "snare", 38 } };
            case 2: return { { "kick", 36 }, { "rim", 38 }, { "perc", 82 } };
            case 3: return {};
            default: return { { "Kick", 36 }, { "snare", 36 } };
        }
    }

    std::string notesText (Rng& rng)
    {
        static const std::array<std::string, 7> texts {
            "@length 1bar\n\n1.1 C2 1/4 v100\n",
            "@length 2bars\n\n1.1 E2 1/8 v90\n1.3 G2 1/8 v80\n",
            "@length 4bars\n",
            "@length 1bar\n\n1.1 kick 1/16 v100\n1.3 snare 1/16 v90\n",
            "1.1 D2 1/4 v70\n",
            "1.1 H2 1/4 v100\n",
            "@length 1bar\n\n1.1 C2 1/4 v100\n2.1 C2 1/4 v100\n",
        };

        return pick (rng, texts);
    }

    TrackPlace place (Rng& rng, const project::Project& project)
    {
        switch (randomInt (rng, 0, 3))
        {
            case 0: return {};
            case 1: return { trackId (rng, project), {} };
            case 2: return { {}, trackId (rng, project) };
            default: return { trackId (rng, project), trackId (rng, project) };
        }
    }

    Command randomCommand (Rng& rng, const project::Project& project)
    {
        switch (randomInt (rng, 0, 15))
        {
            case 0: return SetTempo { pick (rng, std::array<double, 6> { 60, 90, 120.5, 140, 10, 1000 }), gesture (rng) };
            case 1: return SetTimeSignature { pick (rng, std::array<std::string, 9> { "4/4", "2/4", "8/8", "3/4", "6/8", "12/8", "5/4", "4/2", "6/3" }) };

            case 2:
                return AddTrack { pick (rng, std::array<std::string, 3> { "midi", "drums", "audio" }),
                                  pick (rng, std::array<std::string, 5> { "Keys", "Drums 2", "Pad", "", "Bad\nname" }),
                                  chance (rng, 20) ? pick (rng, std::array<std::string, 3> { "trk-keys", "trk-bass", "keys" }) : std::string(),
                                  place (rng, project),
                                  instrument (rng) };

            case 3: return RemoveTrack { trackId (rng, project) };
            case 4: return RenameTrack { trackId (rng, project), pick (rng, std::array<std::string, 3> { "Sub", "Größe", "" }) };
            case 5: return MoveTrack { trackId (rng, project), place (rng, project) };

            case 6:
                return SetMix { trackId (rng, project),
                                maybe (rng, 60, pick (rng, std::array<std::string, 4> { "-6dB", "0dB", "+3dB", "+12dB" })),
                                maybe (rng, 40, pick (rng, std::array<std::string, 3> { "L20", "C", "l20" })),
                                chance (rng, 30) ? std::optional<bool> (chance (rng, 50)) : std::nullopt,
                                chance (rng, 20) ? std::optional<bool> (chance (rng, 50)) : std::nullopt,
                                gesture (rng) };

            case 7: return SetInstrument { trackId (rng, project), instrument (rng) };
            case 8: return SetDrumMap { trackId (rng, project), drumMap (rng) };

            case 9:
            case 10:
            {
                ClipContent content;

                switch (randomInt (rng, 0, 3))
                {
                    case 0: content = EmptyClip { randomInt (rng, 0, 4) }; break;
                    case 1: content = ClipText { notesText (rng) }; break;
                    case 2: content = LinkedTo { clipId (rng, project) }; break;
                    default: content = CopyOf { clipId (rng, project) }; break;
                }

                return AddClip { trackId (rng, project), position (rng), std::move (content),
                                 chance (rng, 10) ? pick (rng, std::array<std::string, 2> { "clp-new", "clp-a" }) : std::string(),
                                 chance (rng, 10) ? pick (rng, std::array<std::string, 2> { "clips/new.notes", "clips/a.notes" }) : std::string() };
            }

            case 11: return MoveClip { clipId (rng, project), position (rng), chance (rng, 40) ? trackId (rng, project) : std::string() };
            case 12: return RemoveClip { clipId (rng, project) };
            case 13: return MakeClipUnique { clipId (rng, project), {} };
            default: return WriteClip { clipId (rng, project), notesText (rng) };
        }
    }

    // Every clip's file is in notesFiles, and nothing else is (Project.h).
    bool filesMatchClips (const project::Project& project)
    {
        std::set<std::string> used;

        for (const auto& track : project.tracks)
            for (const auto& clip : track.clips)
                used.insert (clip.file);

        std::set<std::string> held;

        for (const auto& entry : project.notesFiles)
            held.insert (entry.first);

        return used == held;
    }
}

TEST_CASE ("random command sequences: refusals change nothing, every project reads back, and undo and redo go all the way")
{
    std::map<std::size_t, std::pair<int, int>> outcomes; // by command kind: applied, refused
    int undos = 0;
    int redos = 0;

    for (std::uint32_t seed = 1; seed <= 200; ++seed)
    {
        CAPTURE (seed);
        Rng rng { seed };
        const auto original = makeProject();
        CommandBus bus (original);

        for (int step = 0; step < 40; ++step)
        {
            CAPTURE (step);
            const auto author = chance (rng, 50) ? lee : claude;

            if (chance (rng, 8))
            {
                undos += bus.undo (author, UndoScope::anyAuthor).ok ? 1 : 0;
                continue;
            }

            if (chance (rng, 7))
            {
                redos += bus.redo (author).ok ? 1 : 0;
                continue;
            }

            const auto command = randomCommand (rng, bus.project());
            const auto before = bus.project();
            const auto history = bus.history();
            const auto result = bus.execute (command, author);
            CAPTURE (result.message);

            if (result.ok)
            {
                ++outcomes[command.index()].first;
                REQUIRE (filesMatchClips (bus.project()));
                checkReadsBack (bus.project());
            }
            else
            {
                ++outcomes[command.index()].second;
                REQUIRE (bus.project() == before);
                REQUIRE (bus.history() == history);
            }
        }

        const auto end = bus.project();
        const auto entries = bus.history().size();

        while (! bus.history().empty())
            REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);

        REQUIRE (bus.project() == original);

        for (std::size_t i = 0; i < entries; ++i)
            REQUIRE (bus.redo (lee).ok);

        REQUIRE (bus.project() == end);
    }

    // Every command must get real exercise both ways, or the test proves little.
    for (const auto& [kind, counts] : outcomes)
    {
        CAPTURE (kind);
        MESSAGE ("command " << kind << ": applied " << counts.first << ", refused " << counts.second);
        CHECK (counts.first >= 20);
        CHECK (counts.second >= 20);
    }

    CHECK (outcomes.size() == 14); // every project command (not play and stop)
    MESSAGE ("undos: " << undos << ", redos: " << redos);
    CHECK (undos >= 200);
    CHECK (redos >= 50);
}
