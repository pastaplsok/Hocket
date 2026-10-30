#include "core/Paths.h"
#include "core/project/Project.h"

#include <doctest/doctest.h>

#include <fstream>
#include <sstream>

using namespace hocket::project;

namespace
{
    using Files = std::map<std::string, std::string>;

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

    Project parseOrFail (const Files& files)
    {
        auto result = parseFiles (files);

        for (const auto& error : result.errors)
            MESSAGE (formatDiagnostic (error));

        REQUIRE (result.project);
        return std::move (*result.project);
    }

    struct Position
    {
        int line = 0;
        int column = 0;
    };

    // Line and column of the first occurrence of needle.
    Position positionOf (std::string_view text, std::string_view needle)
    {
        const auto offset = text.find (needle);
        REQUIRE_MESSAGE (offset != std::string_view::npos, needle);

        Position position { 1, 1 };

        for (std::size_t i = 0; i < offset; ++i)
        {
            if (text[i] == '\n')
            {
                ++position.line;
                position.column = 1;
            }
            else
            {
                ++position.column;
            }
        }

        return position;
    }

    bool contains (const std::string& text, std::string_view part)
    {
        return text.find (part) != std::string::npos;
    }

    // The error that matches message, which must be one of the errors.
    Diagnostic errorMatching (const LoadResult& result, std::string_view message)
    {
        CHECK_FALSE (result.project);

        for (const auto& error : result.errors)
            if (contains (error.message, message))
                return error;

        for (const auto& error : result.errors)
            MESSAGE ("got: " << formatDiagnostic (error));

        FAIL ("no error contains: " << message);
        return {};
    }

    std::string replaced (std::string text, std::string_view from, std::string_view to)
    {
        const auto at = text.find (from);
        REQUIRE_MESSAGE (at != std::string::npos, from);
        return text.replace (at, from.size(), to);
    }

    std::string readFile (const std::filesystem::path& path)
    {
        std::ifstream in (path, std::ios::binary);
        std::ostringstream contents;
        contents << in.rdbuf();
        return contents.str();
    }

    const std::string canonicalProject = R"({
  "format": "hocket-project",
  "formatVersion": 0,
  "tempo": 97.5,
  "timeSignature": "4/4",
  "tracks": [
    {
      "id": "trk-bass",
      "name": "Bass \"low\" — main",
      "type": "midi",
      "devices": [
        {
          "id": "dev-bass-synth",
          "type": "tracktion:4osc"
        }
      ],
      "clips": [
        {
          "id": "clp-bass-verse",
          "file": "clips/bass-verse.notes",
          "start": "1.1"
        },
        {
          "id": "clp-bass-chorus",
          "file": "clips/bass-chorus.notes",
          "start": "3.1"
        }
      ]
    },
    {
      "id": "trk-lead",
      "name": "Lead",
      "type": "midi",
      "devices": [
        {
          "id": "dev-lead-synth",
          "type": "tracktion:4osc"
        }
      ],
      "clips": []
    }
  ]
}
)";

    const Files canonicalFiles {
        { "project.json", canonicalProject },
        { "clips/bass-verse.notes", "@length 2bars\n\n1.1      C2    1/4   v100\n" },
        { "clips/bass-chorus.notes", "# chorus\n@length 1bar\n\n1.1      G1    1/2   v110\n" },
    };
}

TEST_CASE ("a canonical project round-trips byte for byte, every file")
{
    CHECK (write (parseOrFail (canonicalFiles)) == canonicalFiles);
}

TEST_CASE ("the loader builds the project model")
{
    const auto project = parseOrFail (canonicalFiles);

    CHECK (project.tempo == 97.5);
    CHECK (project.meter.beatsPerBar == 4);
    REQUIRE (project.tracks.size() == 2);

    const auto& bass = project.tracks[0];
    CHECK (bass.id == "trk-bass");
    CHECK (bass.name == "Bass \"low\" — main");
    CHECK (bass.type == TrackType::midi);
    REQUIRE (bass.devices.size() == 1);
    CHECK (bass.devices[0].id == "dev-bass-synth");
    CHECK (bass.devices[0].type == DeviceType::fourOsc);
    REQUIRE (bass.clips.size() == 2);
    CHECK (bass.clips[1].id == "clp-bass-chorus");
    CHECK (bass.clips[1].file == "clips/bass-chorus.notes");
    CHECK (bass.clips[1].start == 2 * 3840);
    CHECK (bass.clips[1].content.lengthInBars == 1);
    CHECK (bass.clips[1].content.notes.size() == 1);

    CHECK (project.tracks[1].clips.empty());
}

TEST_CASE ("loose project.json is rewritten in canonical form")
{
    Files files = canonicalFiles;
    files["project.json"] =
        R"({"tracks": [{"clips": [{"start": "3.1", "file": "clips/bass-chorus.notes", "id": "clp-bass-chorus"},)"
        R"( {"id": "clp-bass-verse", "start": "1.1.1", "file": "clips/bass-verse.notes"}],)"
        R"( "devices": [{"type": "tracktion:4osc", "id": "dev-bass-synth"}], "type": "midi", "name": "Bass \"low\" — main", "id": "trk-bass"},)"
        R"( {"id": "trk-lead", "name": "Lead", "type": "midi", "devices": [{"id": "dev-lead-synth", "type": "tracktion:4osc"}], "clips": []}],)"
        R"( "timeSignature": "4/4", "tempo": 97.50, "formatVersion": 0, "format": "hocket-project"})";

    CHECK (write (parseOrFail (files)) == canonicalFiles);
}

TEST_CASE ("whole-number tempos are written without a decimal point")
{
    auto files = canonicalFiles;
    files["project.json"] = replaced (canonicalProject, "97.5", "120.0");

    CHECK (contains (write (parseOrFail (files)).at ("project.json"), "\"tempo\": 120,\n"));
}

TEST_CASE ("the example project loads from disk and is in canonical form")
{
    const auto folder = hocket::pathFromUtf8 (HOCKET_EXAMPLES_DIR "/m0-demo");
    const auto result = load (folder);

    for (const auto& error : result.errors)
        MESSAGE (formatDiagnostic (error));

    REQUIRE (result.project);
    CHECK (result.project->tracks.size() == 1);

    // Byte for byte against the files as checked out, which .gitattributes keeps LF.
    for (const auto& [path, contents] : write (*result.project))
    {
        CAPTURE (path);
        CHECK (readFile (folder / hocket::pathFromUtf8 (path)) == contents);
    }
}

TEST_CASE ("loading a folder that isn't a project explains why")
{
    const auto missing = load (hocket::pathFromUtf8 (HOCKET_EXAMPLES_DIR "/no-such-project"));
    REQUIRE (missing.errors.size() == 1);
    CHECK (contains (missing.errors[0].message, "isn't a folder; a Hocket project is a folder containing project.json"));

    const auto noProjectFile = load (hocket::pathFromUtf8 (HOCKET_EXAMPLES_DIR "/m0-demo/clips"));
    REQUIRE (noProjectFile.errors.size() == 1);
    CHECK (noProjectFile.errors[0].file == "project.json");
    CHECK (contains (formatDiagnostic (noProjectFile.errors[0]), "project.json: there's no project.json in"));
}

TEST_CASE ("errors in project.json point at the value, say what's wrong, and show a valid example")
{
    struct Case
    {
        std::string_view from; // edit made to the canonical project
        std::string_view to;
        std::string_view at;   // where the error should point, in the edited text
        std::string_view message;
    };

    const Case cases[] = {
        { R"("hocket-project")", R"("daw-project")", R"("daw-project")", R"("format" must be "hocket-project")" },
        { R"("formatVersion": 0)", R"("formatVersion": 7)", "7,", "format version 7, but this build of Hocket reads only format version 0" },
        { R"("formatVersion": 0)", R"("formatVersion": "zero")", R"("zero")", R"("formatVersion" should be a whole number, e.g. 0)" },
        { "97.5", "1000", "1000", R"("tempo" must be between 20 and 999 BPM, e.g. 120)" },
        { "97.5", R"("fast")", R"("fast")", R"("tempo" should be a number of beats per minute)" },
        { R"("4/4")", R"("3/4")", R"("3/4")", R"(Milestone 0 supports only "4/4" time)" },
        { R"("4/4")", R"("waltz")", R"("waltz")", R"("waltz" isn't a time signature; write it like "4/4")" },
        { R"("tempo")", R"("tempoo")", R"("tempoo")", R"(unknown key "tempoo" (did you mean "tempo"?); the keys are "format", "formatVersion")" },
        { R"("tempo")", R"("tempoo")", "{", R"(the project: missing "tempo"; add e.g. "tempo": 120)" },
        { R"("tempo": 97.5,)", R"("tempo": 97.5, "tempo": 90,)", R"("tempo": 90)", R"("tempo" appears twice; keep only one)" },
        { R"("trk-bass")", R"("bass")", R"("bass")", "tracks[0]: 'bass' isn't a valid track ID: track IDs start with 'trk-', e.g. 'trk-bass'" },
        { R"("trk-bass")", R"("clp-bass")", R"("clp-bass")", "'clp-bass' is a clip ID; a track ID starts with 'trk-'" },
        { R"("trk-bass")", R"("trk-Bass")", R"("trk-Bass")", "after 'trk-', use lowercase letters and digits in words joined by single hyphens" },
        { R"("trk-lead")", R"("trk-bass")", R"("trk-bass",
      "name": "Lead")", "ID 'trk-bass' is already used on line 8; IDs must be unique across the project" },
        { R"("dev-lead-synth")", R"("dev-bass-synth")", R"("dev-bass-synth",
          "type": "tracktion:4osc"
        }
      ],
      "clips": [])", "ID 'dev-bass-synth' is already used on line 13" },
        { R"("midi")", R"("audio")", R"("audio")", R"(track 'trk-bass': "audio" tracks aren't supported yet; Milestone 0 has only "midi" tracks)" },
        { R"("Bass \"low\" — main")", "42", "42", R"(track 'trk-bass': "name" should be a string, e.g. "Bass")" },
        { R"("tracktion:4osc")", R"("vst3:serum")", R"("vst3:serum")", R"(device 'dev-bass-synth': "vst3:serum" isn't available; Milestone 0 has only "tracktion:4osc")" },
        { R"("clips/bass-verse.notes")", R"("clips/Bass Verse.notes")", R"("clips/Bass Verse.notes")", "'clips/Bass Verse.notes' isn't a valid clip file: clip files are 'clips/<name>.notes'" },
        { R"("clips/bass-verse.notes")", R"("../secret.notes")", R"("../secret.notes")", "isn't a valid clip file" },
        { R"("clips/bass-verse.notes")", R"("clips/missing.notes")", R"("clips/missing.notes")", "clip 'clp-bass-verse': 'clips/missing.notes' doesn't exist in the project folder" },
        { R"("clips/bass-chorus.notes")", R"("clips/bass-verse.notes")", R"("clips/bass-verse.notes",
          "start": "3.1")", "'clips/bass-verse.notes' is already used by clip 'clp-bass-verse'; in Milestone 0 each clip needs its own file (linked clips that share one come in v1)" },
        { R"("start": "1.1")", R"("start": "1.5")", R"("1.5")", "clip 'clp-bass-verse': '1.5' isn't a valid position: beat 5 doesn't exist in 4/4" },
        { R"("start": "3.1")", R"("start": "2.3")", R"("2.3")", "clip 'clp-bass-chorus': starts at 2.3, inside clip 'clp-bass-verse' (1.1 to 3.1); clips on a track can't overlap, so start it at 3.1 or later" },
        { R"("start": "1.1")", R"("stat": "1.1")", R"("stat")", R"(unknown key "stat" (did you mean "start"?))" },
        { R"("clips": [])", R"("clips": {})", "{}", R"(track 'trk-lead': "clips" should be an array, e.g. [])" },
        { R"("clips": [])", R"("clips": [7])", "7]", R"(track 'trk-lead', clips[0] should be an object: { "id": "clp-bass-verse", "file": "clips/bass-verse.notes", "start": "1.1" })" },
    };

    for (const auto& c : cases)
    {
        CAPTURE (c.to);
        auto files = canonicalFiles;
        files["project.json"] = replaced (canonicalProject, c.from, c.to);

        const auto error = errorMatching (parseFiles (files), c.message);
        const auto expected = positionOf (files["project.json"], c.at);
        CHECK (error.file == "project.json");
        CHECK (error.line == expected.line);
        CHECK (error.column == expected.column);
    }
}

TEST_CASE ("a track needs exactly one device in Milestone 0")
{
    auto files = canonicalFiles;
    files["project.json"] = replaced (canonicalProject, R"([
        {
          "id": "dev-lead-synth",
          "type": "tracktion:4osc"
        }
      ])", "[]");

    const auto error = errorMatching (parseFiles (files), "track 'trk-lead': a track needs exactly one device in Milestone 0");
    CHECK (error.line == positionOf (files["project.json"], "[]").line);
}

TEST_CASE ("invalid JSON is reported with its position and a hint")
{
    const auto errorFor = [] (const std::string& json)
    {
        const auto result = parseFiles ({ { "project.json", json } });
        REQUIRE (result.errors.size() == 1);
        return result.errors[0];
    };

    const auto trailingComma = errorFor ("{\n  \"format\": \"hocket-project\",\n}\n");
    CHECK (trailingComma.line == 3);
    CHECK (trailingComma.column == 1);
    CHECK (contains (trailingComma.message, "project.json isn't valid JSON: "));
    CHECK (contains (trailingComma.message, "(JSON doesn't allow a comma before a closing bracket)"));

    const auto comment = errorFor ("{\n  // tempo\n  \"tempo\": 120\n}\n");
    CHECK (comment.line == 2);
    CHECK (contains (comment.message, "(JSON doesn't allow comments)"));

    const auto notAnObject = errorFor ("[]");
    CHECK (notAnObject.line == 1);
    CHECK (notAnObject.column == 1);
    CHECK (contains (notAnObject.message, R"(the project should be an object: { "format": "hocket-project", "formatVersion": 0)"));

    CHECK (contains (errorFor ("").message, "isn't valid JSON"));
}

TEST_CASE ("errors inside a clip's .notes file name that file")
{
    auto files = canonicalFiles;
    files["clips/bass-chorus.notes"] = "@length 1bar\n\n1.5      G1    1/2   v110\n";

    const auto error = errorMatching (parseFiles (files), "'1.5' isn't a valid position");
    CHECK (error.file == "clips/bass-chorus.notes");
    CHECK (error.line == 3);
    CHECK (error.column == 1);
    CHECK (formatDiagnostic (error).starts_with ("clips/bass-chorus.notes:3:1: '1.5' isn't a valid position"));
}

TEST_CASE ("project.json errors come first, in file order, then the clip files'")
{
    auto files = canonicalFiles;
    files["clips/bass-verse.notes"] = "1.1 H2 1/4 v100\n";
    files["project.json"] = replaced (replaced (canonicalProject, "97.5", "5"), R"("trk-lead")", R"("lead")");

    const auto result = parseFiles (files);
    REQUIRE (result.errors.size() == 3);
    CHECK (result.errors[0].file == "project.json");
    CHECK (contains (result.errors[0].message, "\"tempo\" must be between"));
    CHECK (result.errors[1].file == "project.json");
    CHECK (contains (result.errors[1].message, "'lead' isn't a valid track ID"));
    CHECK (result.errors[2].file == "clips/bass-verse.notes");
}

TEST_CASE ("diagnostics are formatted with as much location as they have")
{
    CHECK (formatDiagnostic ({ "project.json", 4, 12, "bad" }) == "project.json:4:12: bad");
    CHECK (formatDiagnostic ({ "project.json", 0, 0, "bad" }) == "project.json: bad");
    CHECK (formatDiagnostic ({ "", 0, 0, "bad" }) == "bad");
}
