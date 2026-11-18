// Kit manifests and the catalog of installed kits (SPEC §8.4): kit.json is
// read strictly, with each error at its place, and kits are found in the
// bundled folder and the user's, the user's named "user/<folder>".

#include "common/TemporaryFolder.h"
#include "core/Paths.h"
#include "core/kits/Kit.h"
#include "core/kits/KitCatalog.h"
#include "core/project/Rules.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>

using namespace hocket;
using namespace hocket::kits;

namespace
{
    bool contains (const std::string& text, std::string_view part)
    {
        return text.find (part) != std::string::npos;
    }

    const std::string validManifest = R"({
  "format": "hocket-kit",
  "formatVersion": 0,
  "description": "Electronic, 808-style",
  "pieces": [
    { "name": "kick", "note": 36, "file": "kick.wav", "level": "0dB" },
    { "name": "snare", "note": 38, "file": "snare.wav", "level": "-2.5dB" },
    { "name": "hat-closed", "note": 42, "file": "hats/closed.wav", "level": "-4dB" },
    { "name": "hat-open", "note": 46, "file": "hats/open.wav", "level": "-4dB" },
    { "name": "shaker", "note": 82, "file": "shaker.wav", "level": "-6dB" }
  ],
  "chokeGroups": [["hat-closed", "hat-open"], ["kick"]]
}
)";

    std::string replaced (std::string text, std::string_view from, std::string_view to)
    {
        const auto at = text.find (from);
        REQUIRE_MESSAGE (at != std::string::npos, "the manifest doesn't contain: " << std::string (from));
        return text.replace (at, from.size(), to);
    }

    void writeFile (const std::filesystem::path& file, std::string_view text)
    {
        std::filesystem::create_directories (file.parent_path());
        std::ofstream out (file, std::ios::binary);
        out << text;
    }

    // A kit folder with the valid manifest's samples (core only checks they're there).
    void writeKit (const std::filesystem::path& folder, const std::string& manifest = validManifest)
    {
        writeFile (folder / pathFromUtf8 ("kit.json"), manifest);

        for (const auto* sample : { "kick.wav", "snare.wav", "hats/closed.wav", "hats/open.wav", "shaker.wav" })
            writeFile (folder / pathFromUtf8 (sample), "RIFF");
    }
}

TEST_CASE ("a kit's manifest gives its pieces, their levels and its choke groups")
{
    const auto result = parseManifest (validManifest);
    REQUIRE_MESSAGE (result.manifest, (result.errors.empty() ? std::string() : project::formatDiagnostic (result.errors.front())));

    const auto& kit = *result.manifest;
    CHECK (kit.description == "Electronic, 808-style");
    REQUIRE (kit.pieces.size() == 5);
    CHECK (kit.pieces[1] == Piece { "snare", 38, "snare.wav", -2.5 });
    CHECK (kit.pieces[2].file == "hats/closed.wav");

    CHECK (kit.pieceOn (42)->name == "hat-closed");
    CHECK (kit.pieceOn (40) == nullptr);
    CHECK (kit.pieceNamed ("shaker")->note == 82);
    CHECK (kit.pieceNamed ("crash") == nullptr);

    REQUIRE (kit.chokeGroups.size() == 2);
    CHECK (kit.chokeGroupOf ("hat-open") == 0);
    CHECK (kit.chokeGroupOf ("hat-closed") == 0);
    CHECK (kit.chokeGroupOf ("kick") == 1);
    CHECK (kit.chokeGroupOf ("snare") == -1);

    // No choke groups at all is fine.
    CHECK (parseManifest (replaced (validManifest, R"([["hat-closed", "hat-open"], ["kick"]])", "[]")).manifest);
}

TEST_CASE ("errors in a kit's manifest point at the value and say how to fix it")
{
    struct Case
    {
        std::string_view from; // edit made to the valid manifest
        std::string_view to;
        std::string_view at;   // where the error should point, in the edited text
        std::string_view message;
    };

    const Case cases[] = {
        { R"("level": "-6dB" }
  ],)", R"("level": "-6dB" },
  ],)", "]", "kit.json isn't valid JSON: " },
        { R"("hocket-kit")", R"("hocket-project")", R"("hocket-project")",
          "the kit: \"format\" must be \"hocket-kit\"; this doesn't look like a Hocket kit manifest file" },
        { R"("formatVersion": 0)", R"("formatVersion": 1)", "1", "the kit: the kit manifest is format version 1, but this build of Hocket reads only "
                                                                  "format version 0; it was written by a newer Hocket" },
        { R"("description": "Electronic, 808-style",)", "", R"({
  "format")", "the kit: missing \"description\"; add e.g. \"description\": \"Electronic, 808-style\"" },
        { R"("Electronic, 808-style")", R"("")", R"("")", "the kit: \"description\" should be one line of text, 1 to 200 bytes" },
        { R"("Electronic, 808-style")", R"("Electronic\n808")", R"("Electronic\n808")", "should be one line of text" },
        { R"("chokeGroups")", R"("chokegroups")", R"("chokegroups")", "the kit: unknown key \"chokegroups\" (did you mean \"chokeGroups\"?)" },
        { R"([
    { "name": "kick")", R"([
    { "name": "Kick")", R"("Kick")", "pieces[0]: \"name\": 'Kick' isn't a valid drum name" },
        { R"("name": "snare")", R"("name": "kick")", R"("kick", "note": 38)", "pieces[1]: 'kick' is already a piece, on line 6; each piece has a name of its own" },
        { R"("note": 38)", R"("note": 128)", "128", "piece 'snare': \"note\" should be a MIDI note number from 0 to 127, e.g. 36" },
        { R"("note": 38)", R"("note": "D2")", R"("D2")", "\"note\" should be a MIDI note number from 0 to 127, e.g. 36, not a note name: kits use "
                                                           "General MIDI's numbers, and 'D2' is 38" },
        { R"("note": 38)", R"("note": 36)", R"(36, "file": "snare.wav")", "piece 'snare': note 36 is already piece 'kick''s, on line 6; "
                                                                          "a note plays one piece, so give this one another note" },
        { R"("file": "snare.wav")", R"("file": "snare.mp3")", R"("snare.mp3")", "piece 'snare': \"file\": 'snare.mp3' isn't a .wav file; kits' samples "
                                                                                 "are WAV files, 16- or 24-bit, mono or stereo" },
        { R"("file": "snare.wav")", R"("file": "hats\\snare.wav")", R"("hats\\snare.wav")", "has a backslash; write folders with forward slashes" },
        { R"("file": "snare.wav")", R"("file": "../snare.wav")", R"("../snare.wav")", "should name a file inside the kit's folder" },
        { R"("file": "snare.wav")", R"("file": "C:/kits/snare.wav")", R"("C:/kits/snare.wav")", "isn't inside the kit's folder" },
        { R"("level": "-2.5dB")", R"("level": "-infdB")", R"("-infdB")", "piece 'snare': \"level\" is -infdB, so the piece would be silent; "
                                                                         "remove it from the kit instead" },
        { R"("level": "-2.5dB")", R"("level": "loud")", R"("loud")", "piece 'snare': \"level\": " },
        { R"("level": "-2.5dB")", R"("level": -2.5)", "-2.5", "piece 'snare': \"level\" should be a string, e.g. \"0dB\"" },
        { R"(, "level": "-2.5dB")", "", R"({ "name": "snare")", "piece 'snare': missing \"level\"; add e.g. \"level\": \"0dB\"" },
        { R"("level": "-2.5dB")", R"("levle": "-2.5dB")", R"("levle")", "piece 'snare': unknown key \"levle\" (did you mean \"level\"?)" },
        { R"([["hat-closed", "hat-open"], ["kick"]])", R"([["hat-closed", "hat-opne"]])", R"("hat-opne")",
          "the kit: \"chokeGroups\"[0]: there's no piece 'hat-opne' (did you mean 'hat-open'?)" },
        { R"([["hat-closed", "hat-open"], ["kick"]])", R"([["hat-closed", "hat-open"], ["hat-open"]])", R"("hat-open"]])",
          "\"chokeGroups\"[1]: 'hat-open' is already in choke group 0; a piece is in one group at most" },
        { R"([["hat-closed", "hat-open"], ["kick"]])", R"([["kick", "kick"]])", R"("kick"]])", "'kick' appears twice in this group; keep only one" },
        { R"([["hat-closed", "hat-open"], ["kick"]])", R"([[]])", "[]]", "\"chokeGroups\"[0] should be a list of the pieces that cut each other off" },
        { R"([["hat-closed", "hat-open"], ["kick"]])", R"(["kick"])", R"("kick"])", "\"chokeGroups\"[0] should be a list of the pieces" },
    };

    for (const auto& c : cases)
    {
        const auto text = replaced (validManifest, c.from, c.to);
        INFO ("edited to: " << std::string (c.to));

        const auto result = parseManifest (text);
        CHECK_FALSE (result.manifest);
        REQUIRE_FALSE (result.errors.empty());

        // Some edits make two errors (an unknown key, and the one it was meant to be missing).
        const auto found = std::ranges::find_if (result.errors, [&c] (const auto& e) { return contains (e.message, c.message); });
        REQUIRE_MESSAGE (found != result.errors.end(), "first error: " << result.errors.front().message);

        const auto& error = *found;
        CHECK (error.file == "kit.json");

        // The line and column of the value the error is about.
        const auto at = text.find (c.at);
        REQUIRE (at != std::string::npos);
        const auto line = 1 + static_cast<int> (std::count (text.begin(), text.begin() + static_cast<std::ptrdiff_t> (at), '\n'));
        const auto lineStart = text.rfind ('\n', at == 0 ? 0 : at - 1);
        const auto column = static_cast<int> (at - (lineStart == std::string::npos ? 0 : lineStart + 1)) + 1;
        CHECK_MESSAGE ((error.line == line && error.column == column),
                       "at " << error.line << ":" << error.column << ", expected " << line << ":" << column << ": " << error.message);
    }

    // No pieces at all.
    const auto empty = parseManifest (R"({ "format": "hocket-kit", "formatVersion": 0, "description": "Nothing", "pieces": [], "chokeGroups": [] })");
    REQUIRE (empty.errors.size() == 1);
    CHECK (contains (empty.errors.front().message, "the kit: \"pieces\" is empty; a kit needs at least one"));
}

TEST_CASE ("a sample's file is a .wav inside the kit's folder")
{
    for (const auto* file : { "kick.wav", "Kick 01.WAV", "hats/closed.wav", "ünï ελλ テスト.wav", "a/b/c.Wav" })
    {
        INFO (file);
        CHECK_FALSE (sampleFileProblem (file));
    }

    for (const auto* file : { "", ".wav", "kick", "kick.wav.txt", "/kick.wav", "C:kick.wav", "hats//closed.wav", "./kick.wav", "hats/../kick.wav",
                              "hats\\closed.wav", "kick?.wav", "kick\t.wav" })
    {
        INFO (file);
        CHECK (sampleFileProblem (file));
    }
}

TEST_CASE ("kit names: a bundled kit by its folder's name, a user kit by \"user/\" and its folder's")
{
    using namespace project;

    CHECK_FALSE (kitNameProblem ("acoustic"));
    CHECK_FALSE (kitNameProblem ("user/dusty-909"));
    CHECK (kitNameProblem ("user/"));
    CHECK (kitNameProblem ("user/Dusty"));
    CHECK (kitNameProblem ("users/dusty"));
    CHECK (kitNameProblem ("user/user/dusty"));
    CHECK (kitNameProblem ("/dusty"));
    CHECK_FALSE (kitNameProblem ("user/" + std::string (maxKitNameLength, 'a')));
    CHECK (kitNameProblem ("user/" + std::string (maxKitNameLength + 1, 'a')));

    CHECK (isUserKit ("user/dusty-909"));
    CHECK_FALSE (isUserKit ("acoustic"));
    CHECK (kitFolderName ("user/dusty-909") == "dusty-909");
    CHECK (kitFolderName ("acoustic") == "acoustic");
}

TEST_CASE ("the catalog finds bundled kits by name, and the user's by \"user/\" and their folder's name")
{
    const testing::TemporaryFolder temp (testing::unicodeName);
    const auto bundled = temp.path / pathFromUtf8 ("kits");
    const auto user = temp.path / pathFromUtf8 ("Documents ünï") / pathFromUtf8 ("Hocket") / pathFromUtf8 ("Kits");

    writeKit (bundled / pathFromUtf8 ("electronic"));
    writeKit (user / pathFromUtf8 ("dusty-909"), replaced (validManifest, "Electronic, 808-style", "My 909, dusty"));
    writeKit (user / pathFromUtf8 ("electronic"), replaced (validManifest, "Electronic, 808-style", "Mine, not the bundled one"));
    writeKit (user / pathFromUtf8 ("Kick Pack"));
    writeKit (user / pathFromUtf8 ("broken"), replaced (validManifest, R"("note": 38)", R"("note": 300)"));
    writeKit (user / pathFromUtf8 ("no-shaker"));
    std::filesystem::remove (user / pathFromUtf8 ("no-shaker") / pathFromUtf8 ("shaker.wav"));
    std::filesystem::create_directories (user / pathFromUtf8 ("no-manifest"));

    KitCatalog catalog (bundled, user);

    SUBCASE ("bundled and user kits, never confused")
    {
        const auto electronic = catalog.find ("electronic");
        REQUIRE (electronic.kit != nullptr);
        CHECK (electronic.kit->source == Source::bundled);
        CHECK (electronic.kit->manifest.description == "Electronic, 808-style");
        CHECK (electronic.kit->folder == bundled / pathFromUtf8 ("electronic"));
        CHECK (KitCatalog::describeSource (*electronic.kit) == "bundled");

        // The user's kit of the same folder name is a different kit.
        const auto mine = catalog.find ("user/electronic");
        REQUIRE (mine.kit != nullptr);
        CHECK (mine.kit->source == Source::user);
        CHECK (mine.kit->manifest.description == "Mine, not the bundled one");

        const auto dusty = catalog.find ("user/dusty-909");
        REQUIRE (dusty.kit != nullptr);
        CHECK (dusty.kit->name == "user/dusty-909");
        CHECK (KitCatalog::describeSource (*dusty.kit) == "the user's own, in " + utf8FromPath (user / pathFromUtf8 ("dusty-909")));

        std::vector<std::string> names;

        for (const auto* kit : catalog.kits())
            names.push_back (kit->name);

        CHECK (names == std::vector<std::string> { "electronic", "user/dusty-909", "user/electronic" });
    }

    SUBCASE ("a kit that isn't installed: what is, and a suggestion")
    {
        const auto found = catalog.find ("dusty-909");
        CHECK (found.kit == nullptr);
        CHECK (found.problem == "kit 'dusty-909' isn't installed (did you mean 'user/dusty-909'?). The installed kits are 'electronic' (bundled), "
                                "'user/dusty-909' (the user's own), 'user/electronic' (the user's own)");

        CHECK (contains (catalog.find ("acoustic").problem, "kit 'acoustic' isn't installed. The installed kits are"));
        CHECK (contains (catalog.find ("electronik").problem, "(did you mean 'electronic'?)"));
    }

    SUBCASE ("a broken kit says why, and folders Hocket can't use are listed")
    {
        const auto broken = catalog.find ("user/broken");
        CHECK (broken.kit == nullptr);
        CHECK (contains (broken.problem, "kit 'user/broken' can't be used: " + utf8FromPath (user / pathFromUtf8 ("broken")) + ": kit.json has an error: kit.json:7:"));
        CHECK (contains (broken.problem, "\"note\" should be a MIDI note number from 0 to 127"));

        CHECK (contains (catalog.find ("user/no-shaker").problem, "a sample it names isn't in its folder: 'shaker.wav' (piece 'shaker')"));
        CHECK (contains (catalog.find ("user/no-manifest").problem, "it has no kit.json"));

        const auto problems = catalog.problems();
        REQUIRE (problems.size() == 4);
        CHECK (contains (problems[0], "kit 'user/broken' can't be used"));
        CHECK (contains (problems[3], "'" + utf8FromPath (user / pathFromUtf8 ("Kick Pack")) + "' isn't a kit Hocket can use: 'Kick Pack' isn't a valid kit name: "));
        CHECK (contains (problems[3], "; rename the folder"));
    }

    SUBCASE ("a kit added during the session is found; one already read isn't read again")
    {
        writeKit (user / pathFromUtf8 ("late"));
        writeFile (bundled / pathFromUtf8 ("electronic") / pathFromUtf8 ("kit.json"), replaced (validManifest, "Electronic, 808-style", "Changed"));

        const auto late = catalog.find ("user/late");
        REQUIRE (late.kit != nullptr);
        CHECK (catalog.find ("electronic").kit->manifest.description == "Electronic, 808-style");

        // A broken kit, fixed, is read again.
        writeKit (user / pathFromUtf8 ("broken"));
        CHECK (catalog.find ("user/broken").kit != nullptr);
    }
}

TEST_CASE ("with no kits installed, the catalog says where they go")
{
    const testing::TemporaryFolder temp (testing::unicodeName);
    KitCatalog catalog (temp.path / pathFromUtf8 ("kits"), temp.path / pathFromUtf8 ("Kits"));

    const auto found = catalog.find ("acoustic");
    CHECK (found.kit == nullptr);
    CHECK (found.problem == "kit 'acoustic' isn't installed. No kits are installed: bundled kits go in " + utf8FromPath (temp.path / pathFromUtf8 ("kits"))
                                + ", and the user's own in " + utf8FromPath (temp.path / pathFromUtf8 ("Kits")) + ", named \"user/<folder>\"");
    CHECK (catalog.problems().empty());

    // A catalog with no folders, as tests make, has only what's added.
    KitCatalog empty;
    CHECK (contains (empty.find ("acoustic").problem, "No kits are installed: bundled kits go in kits\\ next to Hocket's programs"));

    empty.add ({ "user/mine", Source::bundled, {}, *parseManifest (validManifest).manifest });
    REQUIRE (empty.find ("user/mine").kit != nullptr);
    CHECK (empty.find ("user/mine").kit->source == Source::user);
}
