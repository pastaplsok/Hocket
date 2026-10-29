// Property and fuzz tests for the .notes serializer (SPEC §3.3). Seeds are
// fixed so failures reproduce; the seed is printed on failure.

#include "core/notes/Notes.h"
#include "core/notes/NotesSyntax.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <set>
#include <tuple>

using namespace hocket::notes;

namespace
{
    using Rng = std::mt19937;

    int randomInt (Rng& rng, int low, int high)
    {
        return std::uniform_int_distribution<int> (low, high) (rng);
    }

    bool oneIn (Rng& rng, int n)
    {
        return randomInt (rng, 1, n) == 1;
    }

    // Printable text without trailing whitespace, as the parser would keep it.
    std::string randomCommentText (Rng& rng)
    {
        static constexpr std::string_view characters = " abcxyz#[]/.+-0123456789@—";
        std::string text = oneIn (rng, 4) ? "" : " ";
        const auto size = randomInt (rng, 0, 12);

        for (int i = 0; i < size; ++i)
            text += characters[static_cast<std::size_t> (randomInt (rng, 0, static_cast<int> (characters.size()) - 1))];

        while (! text.empty() && text.back() == ' ')
            text.pop_back();

        return text;
    }

    std::vector<std::string> randomCommentLines (Rng& rng)
    {
        std::vector<std::string> lines;

        while (oneIn (rng, 3))
            lines.push_back (randomCommentText (rng));

        return lines;
    }

    Comments randomComments (Rng& rng)
    {
        Comments comments { randomCommentLines (rng), {} };

        if (oneIn (rng, 3))
            comments.trailing = randomCommentText (rng);

        return comments;
    }

    Ticks randomLength (Rng& rng)
    {
        static constexpr Ticks common[] = { 60, 120, 240, 320, 480, 640, 960, 1440, 1920, 3840 };

        if (oneIn (rng, 5))
            return randomInt (rng, 1, 5000);

        return common[randomInt (rng, 0, 9)];
    }

    // A valid clip: notes inside the clip, no two at the same position and pitch.
    Clip randomClip (Rng& rng)
    {
        Clip clip;
        clip.lengthInBars = randomInt (rng, 1, 8);
        clip.header = randomCommentLines (rng);
        clip.lengthComments.trailing = oneIn (rng, 4) ? std::optional (randomCommentText (rng)) : std::nullopt;
        clip.footer = randomCommentLines (rng);

        const Ticks clipEnd = clip.lengthInBars * Meter {}.ticksPerBar();
        std::set<std::pair<Ticks, int>> used;
        const auto noteCount = randomInt (rng, 0, 40);

        for (int i = 0; i < noteCount; ++i)
        {
            Note note;
            note.start = oneIn (rng, 3) ? randomInt (rng, 0, static_cast<int> (clipEnd) - 1)
                                        : randomInt (rng, 0, static_cast<int> (clipEnd / 240) - 1) * 240;
            note.pitch = randomInt (rng, 0, 127);

            if (! used.insert ({ note.start, note.pitch }).second)
                continue;

            note.length = randomLength (rng);
            note.velocity = oneIn (rng, 2) ? 100 : randomInt (rng, 1, 127);

            if (isBlackKey (note.pitch))
                note.spelling = oneIn (rng, 2) ? Accidental::sharp : Accidental::flat;

            note.comments = randomComments (rng);
            clip.notes.push_back (note);

            // Chord partners: same start, length and velocity.
            while (oneIn (rng, 3))
            {
                auto partner = note;
                partner.pitch = randomInt (rng, 0, 127);
                partner.spelling = isBlackKey (partner.pitch) ? Accidental::flat : Accidental::natural;
                partner.comments = randomComments (rng);

                if (used.insert ({ partner.start, partner.pitch }).second)
                    clip.notes.push_back (partner);
            }
        }

        return clip;
    }

    using NoteKey = std::tuple<Ticks, int, Ticks, int, Accidental>;

    std::vector<NoteKey> musicOf (const Clip& clip)
    {
        std::vector<NoteKey> keys;

        for (const auto& note : clip.notes)
            keys.emplace_back (note.start, note.pitch, note.length, note.velocity, note.spelling);

        std::ranges::sort (keys);
        return keys;
    }
}

TEST_CASE ("random clips survive write, parse, write unchanged")
{
    for (std::uint32_t seed = 1; seed <= 500; ++seed)
    {
        CAPTURE (seed);
        Rng rng { seed };
        const auto clip = randomClip (rng);

        const auto text = write (clip);
        const auto reparsed = parse (text);

        if (! reparsed.errors.empty())
            FAIL (formatDiagnostic (reparsed.errors.front(), "written") << "\n" << text);

        REQUIRE (reparsed.clip);
        CHECK (write (*reparsed.clip) == text);
        CHECK (musicOf (*reparsed.clip) == musicOf (clip));
        CHECK (reparsed.clip->lengthInBars == clip.lengthInBars);
        CHECK (reparsed.clip->header == clip.header);
        CHECK (reparsed.clip->footer == clip.footer);
    }
}

TEST_CASE ("mangled input never crashes the parser, and anything it accepts is stable")
{
    constexpr std::string_view base = "# header\n@length 2bars\n\n1.1      [C3 Eb3 G3]  1/2   v80  # chord\n"
                                      "1.3.2+10  F#4   1/8t  v100\n\n2.1      C2    250tk v127\n";
    static constexpr std::string_view alphabet = " \t\n\r#[]/.+-@tkvbCDEFGAB0123456789x";
    int accepted = 0;
    int rejected = 0;

    for (std::uint32_t seed = 1; seed <= 2000; ++seed)
    {
        CAPTURE (seed);
        Rng rng { seed };
        std::string text (base);
        // Few edits per input, so a fair share still parses and gets the stability check.
        const auto edits = randomInt (rng, 1, 3);

        for (int i = 0; i < edits; ++i)
        {
            const auto at = static_cast<std::size_t> (randomInt (rng, 0, static_cast<int> (text.size())));
            const auto character = alphabet[static_cast<std::size_t> (randomInt (rng, 0, static_cast<int> (alphabet.size()) - 1))];

            switch (randomInt (rng, 0, 2))
            {
                case 0: text.insert (at, 1, character); break;
                case 1: if (at < text.size()) text.erase (at, 1); break;
                default: if (at < text.size()) text[at] = character; break;
            }
        }

        CAPTURE (text);
        const auto result = parse (text);

        CHECK (result.clip.has_value() == result.errors.empty());

        for (const auto& error : result.errors)
        {
            CHECK (error.line >= 1);
            CHECK (error.column >= 1);
            CHECK_FALSE (error.message.empty());
        }

        if (result.clip)
        {
            ++accepted;
            const auto once = write (*result.clip);
            const auto again = parse (once);
            REQUIRE (again.clip);
            CHECK (write (*again.clip) == once);
        }
        else
        {
            ++rejected;
        }
    }

    // Both paths must get real exercise, or the test proves little.
    MESSAGE ("mangled inputs accepted: " << accepted << ", rejected: " << rejected);
    CHECK (accepted >= 200);
    CHECK (rejected >= 200);
}
