// patch_clip's core (SPEC §6.2): notes matched one by one, removed and added,
// all edits or none, and errors that show the clip's lines.

#include "BusTesting.h"
#include "core/notes/NotesPatch.h"

using namespace hocket;
using namespace hocket::notes;
using namespace hocket::testing;

namespace
{
    // The clip's text after the edits, or the error.
    std::string patched (std::string_view text, std::vector<PatchEdit> edits, const DrumMap* drumMap = nullptr)
    {
        const auto clip = parsedNotes (text, {}, drumMap);
        const auto result = patch (clip, edits, {}, drumMap);

        if (! result.clip)
            return "error: " + result.error;

        return write (*result.clip, {}, drumMap);
    }

    PatchEdit oldOnly (std::string text) { return { std::move (text), std::nullopt }; }
    PatchEdit newOnly (std::string text) { return { std::nullopt, std::move (text) }; }
    PatchEdit oldAndNew (std::string old, std::string fresh) { return { std::move (old), std::move (fresh) }; }

    const std::string bass = "@length 2bars\n\n1.1 C2 1/8 v100\n1.3 E2 1/8 v95  # passing note\n1.4 G2 1/8 v90\n2.1 C2 1/2 v100\n";

    DrumMap drums()
    {
        DrumMap map;
        map.add ("kick", 36);
        map.add ("snare", 38);
        map.add ("hat-closed", 42);
        return map;
    }
}

TEST_CASE ("patch: old notes removed, new ones added, spacing and spelling ignored")
{
    CHECK (patched (bass, { oldAndNew ("1.4    G2 1/8   v90", "1.4 A2 1/8 v90") })
           == "@length 2bars\n\n1.1      C2    1/8   v100\n1.3      E2    1/8   v95  # passing note\n1.4      A2    1/8   v90\n\n"
              "2.1      C2    1/2   v100\n");

    // Only old removes; only new adds, in its place by position.
    CHECK (patched (bass, { oldOnly ("1.1 C2 1/8 v100"), newOnly ("1.2 D2 1/8 v80") })
           == "@length 2bars\n\n1.2      D2    1/8   v80\n1.3      E2    1/8   v95  # passing note\n1.4      G2    1/8   v90\n\n"
              "2.1      C2    1/2   v100\n");

    // A note matches by number, whatever its spelling.
    const auto flat = "@length 1bar\n\n1.1 Eb2 1/4 v100\n";
    CHECK (patched (flat, { oldOnly ("1.1 D#2 1/4 v100") }) == "@length 1bar\n");

    // Each edit sees what the one before left.
    const auto clip = parsedNotes (bass);
    const auto result = patch (clip, std::vector { newOnly ("1.2 D2 1/8 v80"), oldAndNew ("1.2 D2 1/8 v80", "1.2 D2 1/8 v70") }, {}, nullptr);
    REQUIRE (result.clip);
    CHECK (result.summary == "edit 1 added 1 note; edit 2 removed 1 note and added 1");
}

TEST_CASE ("patch: a note in a chord or a merged line matches on its own, and the line's comment stays while any of its notes do")
{
    const auto map = drums();
    const auto beat = "@length 1bar\n\n1.1 [kick hat-closed] 1/16 v100  # downbeat\n1.2 snare 1/16 v100\n";

    CHECK (patched (beat, { oldOnly ("1.1 hat-closed 1/16 v100") }, &map)
           == "@length 1bar\n\n1.1      kick  1/16  v100  # downbeat\n1.2      snare 1/16  v100\n");

    // The last of the line's notes takes the comment with it.
    CHECK (patched (beat, { oldOnly ("1.1 [kick hat-closed] 1/16 v100") }, &map) == "@length 1bar\n\n1.2      snare 1/16  v100\n");

    // A comment in old must be the line's; without one, any matches.
    CHECK (patched (beat, { oldOnly ("1.1 kick 1/16 v100  # downbeat") }, &map)
           == "@length 1bar\n\n1.1      hat-closed  1/16  v100  # downbeat\n1.2      snare 1/16  v100\n");
    CHECK (patched (beat, { oldOnly ("1.1 kick 1/16 v100  # upbeat") }, &map)
           == "error: edit 1: 1.1 kick is in the clip, but with the comment \"# downbeat\", not \"# upbeat\", so nothing was changed. Its line is:\n"
              "1.1      [kick hat-closed]  1/16  v100  # downbeat");

    // Comments in new go with their lines.
    CHECK (patched (beat, { newOnly ("# fill\n1.4.3 snare 1/16 v60  # ghost") }, &map)
           == "@length 1bar\n\n1.1      [kick hat-closed]  1/16  v100  # downbeat\n1.2      snare 1/16  v100\n# fill\n1.4.3    snare 1/16  v60  # ghost\n");
}

TEST_CASE ("patch: a note not found shows the bar's lines now; one found with other values says which")
{
    CHECK (patched (bass, { oldOnly ("1.2 E2 1/8 v95") })
           == "error: edit 1: 1.2 E2 isn't in the clip, so nothing was changed. Its lines in bar 1 now:\n"
              "1.1      C2    1/8   v100\n1.3      E2    1/8   v95  # passing note\n1.4      G2    1/8   v90");

    CHECK (patched ("@length 2bars\n\n2.1 C2 1/4 v100\n", { oldOnly ("1.1 C2 1/4 v100") })
           == "error: edit 1: 1.1 C2 isn't in the clip, so nothing was changed. There are no notes in bar 1 now.");

    CHECK (patched (bass, { oldAndNew ("1.3 E2 1/4 v90", "1.3 F2 1/8 v90") })
           == "error: edit 1: 1.3 E2 is in the clip, but with length 1/8, not 1/4 and velocity v95, not v90, so nothing was changed. Its line is:\n"
              "1.3      E2    1/8   v95  # passing note");

    // A note new would add twice.
    CHECK (patched (bass, { newOnly ("1.1 C2 1/4 v60") })
           == "error: edit 1: 1.1 C2 is in the clip already, so nothing was changed. Its line is:\n1.1      C2    1/8   v100\n"
              "To change it, put it in old as it is, and in new as it should be.");
}

TEST_CASE ("patch: all edits or none, and what can't be read")
{
    // The second edit fails, so the first's change doesn't happen either.
    const auto clip = parsedNotes (bass);
    const auto result = patch (clip, std::vector { oldOnly ("1.1 C2 1/8 v100"), oldOnly ("1.1 C2 1/8 v100") }, {}, nullptr);
    CHECK_FALSE (result.clip);
    CHECK (result.error.starts_with ("edit 2: 1.1 C2 isn't in the clip"));

    CHECK (patched (bass, { PatchEdit {} }) == "error: edit 1 has neither old nor new: give old to remove notes, new to add them, or both to replace them; "
                                               "nothing was changed");
    CHECK (patched (bass, { newOnly ("@length 4bars") })
           == "error: edit 1's new has a directive line, but patch_clip changes notes only: give a new @length with write_clip; nothing was changed");
    CHECK (patched (bass, { oldOnly ("# just a comment") })
           == "error: edit 1's old has no notes: give its lines in .notes form, e.g. \"1.3 E2 1/8 v90\"; nothing was changed");
    CHECK (patched (bass, { newOnly ("3.1 C2 1/4 v100") }).starts_with ("error: edit 1's new can't be read: line 1:"));
    CHECK (patched (bass, { newOnly ("1.1 C2 1/4") }).starts_with ("error: edit 1's new can't be read: line 1:"));
}
