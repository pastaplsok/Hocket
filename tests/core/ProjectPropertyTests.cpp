// Property and fuzz tests for the project serializer (SPEC §3.3). Seeds are
// fixed so failures reproduce; the seed is printed on failure.

#include "core/notes/NotesSyntax.h"
#include "core/project/Project.h"

#include <doctest/doctest.h>

#include <iterator>
#include <limits>
#include <random>
#include <set>

using namespace hocket;
using namespace hocket::project;

namespace
{
    using Rng = std::mt19937;
    using Files = std::map<std::string, std::string>;

    int randomInt (Rng& rng, int low, int high)
    {
        return std::uniform_int_distribution<int> (low, high) (rng);
    }

    LoadResult parseFiles (const Files& files)
    {
        return parse (files.at ("project.json"), [&files] (const std::string& path) -> std::optional<std::string>
        {
            const auto found = files.find (path);

            if (found == files.end())
                return {};

            return found->second;
        });
    }

    std::string randomWord (Rng& rng)
    {
        static constexpr std::string_view letters = "abcdefghijklmnopqrstuvwxyz0123456789";
        std::string word;
        const auto size = randomInt (rng, 1, 6);

        for (int i = 0; i < size; ++i)
            word += letters[static_cast<std::size_t> (randomInt (rng, 0, static_cast<int> (letters.size()) - 1))];

        return word;
    }

    // Display names are free text: quotes, backslashes, control characters, UTF-8.
    std::string randomName (Rng& rng)
    {
        static const std::string_view pieces[] = { "Bass", " ", "\"", "\\", "\t", "\n", "—", "é", "Lead 2", "/", "{}", "" };
        std::string name;
        const auto count = randomInt (rng, 0, 5);

        for (int i = 0; i < count; ++i)
            name += pieces[static_cast<std::size_t> (randomInt (rng, 0, 11))];

        return name;
    }

    // One note per position, so the writer's chord grouping can't reorder them.
    notes::Clip randomNotes (Rng& rng, notes::Meter meter)
    {
        notes::Clip clip;
        clip.lengthInBars = randomInt (rng, 1, 4);
        const auto slots = static_cast<int> (clip.lengthInBars * meter.ticksPerBar() / 240); // sixteenths
        std::set<int> used;

        for (int i = randomInt (rng, 0, 8); i > 0; --i)
            used.insert (randomInt (rng, 0, slots - 1));

        for (const auto slot : used)
        {
            notes::Note note;
            note.start = slot * 240;
            note.length = 240 * randomInt (rng, 1, 8);
            note.pitch = randomInt (rng, 0, 127);
            note.velocity = randomInt (rng, 1, 127);
            note.spelling = notes::isBlackKey (note.pitch) ? notes::Accidental::flat : notes::Accidental::natural;
            clip.notes.push_back (note);
        }

        return clip;
    }

    notes::Meter randomMeter (Rng& rng)
    {
        static constexpr int denominators[] = { 2, 4, 8, 16 };

        if (randomInt (rng, 0, 2) == 0)
            return {};

        return { randomInt (rng, 1, notes::Meter::maxNumerator), denominators[randomInt (rng, 0, 3)] };
    }

    double randomVolume (Rng& rng)
    {
        if (randomInt (rng, 0, 9) == 0)
            return -std::numeric_limits<double>::infinity();

        return randomInt (rng, 0, 2) == 0 ? 0.0 : randomInt (rng, -960, 60) / 10.0;
    }

    Project randomProject (Rng& rng)
    {
        Project project;
        project.tempo = randomInt (rng, 0, 1) == 0 ? randomInt (rng, 20, 999)
                                                   : std::uniform_real_distribution<double> (20.0, 999.0) (rng);
        project.meter = randomMeter (rng);
        std::set<std::string> ids;
        int fileNumber = 0;

        const auto uniqueId = [&] (const std::string& prefix)
        {
            for (;;)
                if (auto id = prefix + randomWord (rng); ids.insert (id).second)
                    return id;
        };

        for (int t = randomInt (rng, 0, 4); t > 0; --t)
        {
            Track track;
            track.id = uniqueId ("trk-");
            track.name = randomName (rng);
            track.volume = randomVolume (rng);
            track.pan = randomInt (rng, 0, 2) == 0 ? 0 : randomInt (rng, -100, 100);
            track.mute = randomInt (rng, 0, 3) == 0;
            track.solo = randomInt (rng, 0, 3) == 0;

            if (randomInt (rng, 0, 4) > 0)
                track.devices.push_back ({ uniqueId ("dev-"), DeviceType::synth });

            notes::Ticks nextFree = 0;

            for (int c = randomInt (rng, 0, 4); c > 0; --c)
            {
                Clip clip;
                clip.id = uniqueId ("clp-");
                clip.start = nextFree + 240 * randomInt (rng, 0, 32);

                // Sometimes linked to an earlier clip: it uses that one's file.
                if (! project.notesFiles.empty() && randomInt (rng, 0, 2) == 0)
                {
                    auto file = project.notesFiles.begin();
                    std::advance (file, randomInt (rng, 0, static_cast<int> (project.notesFiles.size()) - 1));
                    clip.file = file->first;
                }
                else
                {
                    clip.file = "clips/c" + std::to_string (++fileNumber) + ".notes";
                    project.notesFiles[clip.file] = randomNotes (rng, project.meter);
                }

                nextFree = clip.start + project.notesFiles.at (clip.file).lengthInBars * project.meter.ticksPerBar();
                track.clips.push_back (std::move (clip));
            }

            project.tracks.push_back (std::move (track));
        }

        return project;
    }
}

TEST_CASE ("random projects survive write, parse, write unchanged")
{
    for (std::uint32_t seed = 1; seed <= 300; ++seed)
    {
        CAPTURE (seed);
        Rng rng { seed };
        const auto project = randomProject (rng);
        const auto files = write (project);
        const auto reparsed = parseFiles (files);

        if (! reparsed.errors.empty())
            FAIL (formatDiagnostic (reparsed.errors.front()) << "\n" << files.at ("project.json"));

        REQUIRE (reparsed.project);
        CHECK (*reparsed.project == project);
        CHECK (write (*reparsed.project) == files);
    }
}

TEST_CASE ("mangled project.json never crashes the loader, and anything it accepts is stable")
{
    Rng setup { 7 };
    auto project = randomProject (setup);

    while (project.tracks.size() < 2)
        project = randomProject (setup);

    const auto canonical = write (project);
    static constexpr std::string_view alphabet = " \n\"{}[],:0123456789.-abckrtv/\\";
    int accepted = 0;
    int rejected = 0;

    for (std::uint32_t seed = 1; seed <= 1500; ++seed)
    {
        CAPTURE (seed);
        Rng rng { seed };
        auto files = canonical;
        auto& text = files["project.json"];

        for (int edits = randomInt (rng, 1, 2); edits > 0; --edits)
        {
            const auto at = static_cast<std::size_t> (randomInt (rng, 0, static_cast<int> (text.size()) - 1));
            const auto character = alphabet[static_cast<std::size_t> (randomInt (rng, 0, static_cast<int> (alphabet.size()) - 1))];

            switch (randomInt (rng, 0, 2))
            {
                case 0: text.insert (at, 1, character); break;
                case 1: text.erase (at, 1); break;
                default: text[at] = character; break;
            }
        }

        CAPTURE (text);
        const auto result = parseFiles (files);
        CHECK (result.project.has_value() == result.errors.empty());

        for (const auto& error : result.errors)
        {
            CHECK_FALSE (error.message.empty());

            if (error.file == "project.json")
            {
                CHECK (error.line >= 1);
                CHECK (error.column >= 1);
            }
        }

        if (result.project)
        {
            ++accepted;
            const auto once = write (*result.project);
            const auto again = parseFiles (once);
            REQUIRE (again.project);
            CHECK (write (*again.project) == once);
        }
        else
        {
            ++rejected;
        }
    }

    // Both paths must get real exercise, or the test proves little.
    MESSAGE ("mangled projects accepted: " << accepted << ", rejected: " << rejected);
    CHECK (accepted >= 100);
    CHECK (rejected >= 100);
}
