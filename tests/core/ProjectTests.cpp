#include "core/Paths.h"
#include "core/project/Project.h"

#include <doctest/doctest.h>

#include <cmath>
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
      "volume": "0dB",
      "pan": "C",
      "mute": false,
      "solo": false,
      "devices": [
        {
          "id": "dev-bass-synth",
          "type": "hocket:synth"
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
      "volume": "-6.5dB",
      "pan": "L20",
      "mute": true,
      "solo": false,
      "devices": [
        {
          "id": "dev-lead-synth",
          "type": "hocket:synth"
        }
      ],
      "clips": []
    },
    {
      "id": "trk-pad",
      "name": "Pad",
      "type": "midi",
      "volume": "-infdB",
      "pan": "R100",
      "mute": false,
      "solo": true,
      "devices": [],
      "clips": [
        {
          "id": "clp-pad-verse",
          "file": "clips/bass-verse.notes",
          "start": "5.1"
        }
      ]
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
    CHECK (project.meter == hocket::notes::Meter { 4, 4 });
    REQUIRE (project.tracks.size() == 3);

    const auto& bass = project.tracks[0];
    CHECK (bass.id == "trk-bass");
    CHECK (bass.name == "Bass \"low\" — main");
    CHECK (bass.type == TrackType::midi);
    CHECK (bass.volume == 0.0);
    CHECK (bass.pan == 0);
    CHECK_FALSE (bass.mute);
    CHECK_FALSE (bass.solo);
    REQUIRE (bass.devices.size() == 1);
    CHECK (bass.devices[0].id == "dev-bass-synth");
    CHECK (bass.devices[0].type == DeviceType::synth);
    REQUIRE (bass.clips.size() == 2);
    CHECK (bass.clips[1].id == "clp-bass-chorus");
    CHECK (bass.clips[1].file == "clips/bass-chorus.notes");
    CHECK (bass.clips[1].start == 2 * 3840);
    CHECK (notesOf (project, bass.clips[1]).lengthInBars == 1);
    CHECK (notesOf (project, bass.clips[1]).notes.size() == 1);

    const auto& lead = project.tracks[1];
    CHECK (lead.volume == -6.5);
    CHECK (lead.pan == -20);
    CHECK (lead.mute);
    CHECK (lead.clips.empty());

    const auto& pad = project.tracks[2];
    CHECK (std::isinf (pad.volume));
    CHECK (pad.pan == 100);
    CHECK (pad.solo);
    CHECK (pad.devices.empty());

    // Linked: the pad's clip plays the bass verse's file, which is read once.
    REQUIRE (pad.clips.size() == 1);
    CHECK (&notesOf (project, pad.clips[0]) == &notesOf (project, bass.clips[0]));
    CHECK (project.notesFiles.size() == 2);
    CHECK (clipsUsing (project, "clips/bass-verse.notes") == std::vector<const Clip*> { &bass.clips[0], &pad.clips[0] });
    CHECK (clipEnd (project, pad.clips[0]) == 6 * 3840);
}

TEST_CASE ("loose project.json is rewritten in canonical form")
{
    Files files = canonicalFiles;
    files["project.json"] =
        R"({"tracks": [{"clips": [{"start": "3.1", "file": "clips/bass-chorus.notes", "id": "clp-bass-chorus"},)"
        R"( {"id": "clp-bass-verse", "start": "1.1.1", "file": "clips/bass-verse.notes"}], "solo": false, "mute": false, "pan": "C",)"
        R"( "volume": "-0.04 dB", "devices": [{"type": "hocket:synth", "id": "dev-bass-synth"}], "type": "midi", "name": "Bass \"low\" — main", "id": "trk-bass"},)"
        R"( {"id": "trk-lead", "name": "Lead", "type": "midi", "volume": "-6.50dB", "pan": "L020", "mute": true, "solo": false,)"
        R"( "devices": [{"id": "dev-lead-synth", "type": "hocket:synth"}], "clips": []},)"
        R"( {"id": "trk-pad", "name": "Pad", "type": "midi", "volume": "-inf dB", "pan": "R100", "mute": false, "solo": true, "devices": [],)"
        R"( "clips": [{"id": "clp-pad-verse", "file": "clips/bass-verse.notes", "start": "5.1"}]}],)"
        R"( "timeSignature": "4/4", "tempo": 97.50, "formatVersion": 0, "format": "hocket-project"})";

    CHECK (write (parseOrFail (files)) == canonicalFiles);
}

TEST_CASE ("in other time signatures, a beat is the denominator's note")
{
    // In 6/8, clip starts and note positions count eighths, and sixteenths are 1-2.
    auto files = canonicalFiles;
    files["project.json"] = replaced (replaced (canonicalProject, R"("4/4")", R"("6/8")"), R"("start": "3.1")", R"("start": "3.4.2")");
    files["clips/bass-chorus.notes"] = "@length 1bar\n\n1.6.2    G1    1/16  v110\n";

    const auto project = parseOrFail (files);
    CHECK (project.meter == hocket::notes::Meter { 6, 8 });
    CHECK (project.tracks[0].clips[1].start == 2 * 2880 + 3 * 480 + 240);

    const auto& chorus = notesOf (project, project.tracks[0].clips[1]);
    REQUIRE (chorus.notes.size() == 1);
    CHECK (chorus.notes[0].start == 5 * 480 + 240);

    // Round-trips, and @length's bars are 6/8 bars.
    CHECK (write (project) == files);
    CHECK (clipEnd (project, project.tracks[0].clips[1]) == 3 * 2880 + 3 * 480 + 240);

    // A position that doesn't exist in 6/8 names the clip file it's in.
    files["clips/bass-chorus.notes"] = "@length 1bar\n\n1.6.3    G1    1/16  v110\n";
    const auto error = errorMatching (parseFiles (files), "'1.6.3' isn't a valid position: sixteenth 3 doesn't exist in 6/8: "
                                                          "a beat there is an eighth note, so sixteenths are 1-2");
    CHECK (error.file == "clips/bass-chorus.notes");
}

TEST_CASE ("linked clips: a file is read once, its errors reported once, and written once")
{
    auto files = canonicalFiles;
    files["clips/bass-verse.notes"] = "@length 2bars\n\n1.5 C2 1/4 v100\n";

    const auto result = parseFiles (files);
    CHECK_FALSE (result.project);
    REQUIRE (result.errors.size() == 1);
    CHECK (result.errors[0].file == "clips/bass-verse.notes");

    // Three clips, two files.
    CHECK (write (parseOrFail (canonicalFiles)).size() == 3);
}

TEST_CASE ("a song is at most 999 bars long: every clip ends by 1000.1")
{
    auto files = canonicalFiles;
    files["project.json"] = replaced (canonicalProject, R"("start": "3.1")", R"("start": "999.1")");
    CHECK (parseFiles (files).project); // the chorus is one bar long

    files["project.json"] = replaced (canonicalProject, R"("start": "1.1")", R"("start": "999.1")");
    const auto error = errorMatching (parseFiles (files), "clip 'clp-bass-verse': it runs from 999.1 to 1001.1, but a song is at most "
                                                          "999 bars long, so every clip ends by 1000.1; start it at 998.1 or earlier");
    CHECK (error.line == positionOf (files["project.json"], R"("999.1")").line);
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
        { "97.5", "1000", "1000", R"("tempo" must be between 20 and 999 BPM (quarter notes per minute), e.g. 120)" },
        { "97.5", R"("fast")", R"("fast")", R"("tempo" should be a number of quarter notes per minute (BPM), e.g. 120)" },
        { R"("4/4")", R"("6/3")", R"("6/3")", "the project: '6/3' isn't a time signature Hocket supports: the bottom number is 3. The top number is 1 to 32 "
                                              "and the bottom one 2, 4, 8 or 16, e.g. '4/4', '3/4', '6/8', '7/8'" },
        { R"("4/4")", R"("33/16")", R"("33/16")", "'33/16' isn't a time signature Hocket supports: the top number is 33" },
        { R"("4/4")", R"("0/4")", R"("0/4")", "'0/4' isn't a time signature Hocket supports: the top number is 0" },
        { R"("4/4")", R"("waltz")", R"("waltz")", "'waltz' isn't a time signature Hocket supports: it needs a '/' between two numbers" },
        { R"("tempo")", R"("tempoo")", R"("tempoo")", R"(unknown key "tempoo" (did you mean "tempo"?); the keys are "format", "formatVersion")" },
        { R"("tempo")", R"("tempoo")", "{", R"(the project: missing "tempo"; add e.g. "tempo": 120)" },
        { R"("tempo": 97.5,)", R"("tempo": 97.5, "tempo": 90,)", R"("tempo": 90)", R"("tempo" appears twice; keep only one)" },
        { R"("trk-bass")", R"("bass")", R"("bass")", "tracks[0]: 'bass' isn't a valid track ID: track IDs start with 'trk-', e.g. 'trk-bass'" },
        { R"("trk-bass")", R"("clp-bass")", R"("clp-bass")", "'clp-bass' is a clip ID; a track ID starts with 'trk-'" },
        { R"("trk-bass")", R"("trk-Bass")", R"("trk-Bass")", "after 'trk-', use lowercase letters and digits in words joined by single hyphens" },
        { R"("trk-lead")", R"("trk-bass")", R"("trk-bass",
      "name": "Lead")", "ID 'trk-bass' is already used on line 8; IDs must be unique across the project" },
        { R"("dev-lead-synth")", R"("dev-bass-synth")", R"("dev-bass-synth",
          "type": "hocket:synth"
        }
      ],
      "clips": [])", "ID 'dev-bass-synth' is already used on line 17" },
        { R"("midi")", R"("audio")", R"("audio")", R"(track 'trk-bass': "audio" tracks aren't supported yet; so far there are "midi" and "drums" tracks)" },
        { R"("0dB")", R"("+12dB")", R"("+12dB")", R"(track 'trk-bass': "volume": '+12dB' is out of range: volume goes from -96dB to +6dB, or -infdB)" },
        { R"("0dB")", R"("-6")", R"("-6")", R"(track 'trk-bass': "volume": '-6' isn't a valid volume: it has no unit; volume is written in dB)" },
        { R"("0dB")", "-6", "-6", R"(track 'trk-bass': "volume" should be a string, e.g. "0dB")" },
        { R"("L20")", R"("l20")", R"("l20")", R"(track 'trk-lead': "pan": 'l20' isn't a pan position: write it in capitals: 'L20')" },
        { R"("L20")", "-20", "-20", R"(track 'trk-lead': "pan" should be a string, e.g. "C")" },
        { R"("mute": true)", R"("mute": "yes")", R"("yes")", R"(track 'trk-lead': "mute" should be true or false, without quotes)" },
        { R"("solo": true)", R"("solo": 1)", "1,", R"(track 'trk-pad': "solo" should be true or false, without quotes)" },
        { R"("pan": "C",)", "", R"({
      "id": "trk-bass")", R"(track 'trk-bass': missing "pan"; add e.g. "pan": "C")" },
        { R"("volume": "0dB",)", R"("volume": "0dB", "loudness": "0dB",)", R"("loudness")", R"(track 'trk-bass': unknown key "loudness"; the keys are "id", "name", "type", "volume", "pan", "mute", "solo", "devices", "clips")" },
        { R"("Bass \"low\" — main")", "42", "42", R"(track 'trk-bass': "name" should be a string, e.g. "Bass")" },
        { R"("hocket:synth")", R"("vst3:serum")", R"("vst3:serum")", R"(device 'dev-bass-synth': "vst3:serum" isn't available; so far there are "hocket:synth", )"
                                                                     R"(Hocket's synth, and "hocket:drum-sampler", which plays a drum kit)" },
        { R"("hocket:synth")", R"("tracktion:4osc")", R"("tracktion:4osc")", R"(device 'dev-bass-synth': "tracktion:4osc" is now written "hocket:synth")" },
        { R"("clips/bass-verse.notes")", R"("clips/Bass Verse.notes")", R"("clips/Bass Verse.notes")", "'clips/Bass Verse.notes' isn't a valid clip file: clip files are 'clips/<name>.notes'" },
        { R"("clips/bass-verse.notes")", R"("../secret.notes")", R"("../secret.notes")", "isn't a valid clip file" },
        { R"("clips/bass-verse.notes")", R"("clips/missing.notes")", R"("clips/missing.notes")", "clip 'clp-bass-verse': 'clips/missing.notes' doesn't exist in the project folder" },
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

TEST_CASE ("a track has at most one device for now")
{
    auto files = canonicalFiles;
    files["project.json"] = replaced (canonicalProject, R"("devices": [],)", R"("devices": [{ "id": "dev-pad-a", "type": "hocket:synth" }, { "id": "dev-pad-b", "type": "hocket:synth" }],)");

    const auto error = errorMatching (parseFiles (files), "track 'trk-pad': a track has at most one device for now, its instrument (effects come in Milestone 5)");
    CHECK (error.line == positionOf (files["project.json"], "dev-pad-a").line);
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

//==============================================================================
namespace
{
    // Two drums tracks sharing a file, whose maps name its notes alike, and a bass track.
    const std::string drumProject = R"({
  "format": "hocket-project",
  "formatVersion": 0,
  "tempo": 120,
  "timeSignature": "4/4",
  "tracks": [
    {
      "id": "trk-drums",
      "name": "Drums",
      "type": "drums",
      "volume": "0dB",
      "pan": "C",
      "mute": false,
      "solo": false,
      "devices": [
        {
          "id": "dev-drums-kit",
          "type": "hocket:drum-sampler",
          "kit": "acoustic"
        }
      ],
      "drumMap": {
        "kick": 36,
        "snare": 38,
        "hat-closed": 42,
        "crash": 49
      },
      "clips": [
        {
          "id": "clp-beat",
          "file": "clips/beat.notes",
          "start": "1.1"
        }
      ]
    },
    {
      "id": "trk-perc",
      "name": "Percussion",
      "type": "drums",
      "volume": "0dB",
      "pan": "R30",
      "mute": false,
      "solo": false,
      "devices": [],
      "drumMap": {
        "kick": 36,
        "snare": 38,
        "cowbell": 56
      },
      "clips": [
        {
          "id": "clp-perc",
          "file": "clips/beat.notes",
          "start": "2.1"
        }
      ]
    },
    {
      "id": "trk-bass",
      "name": "Bass",
      "type": "midi",
      "volume": "0dB",
      "pan": "C",
      "mute": false,
      "solo": false,
      "devices": [
        {
          "id": "dev-bass-synth",
          "type": "hocket:synth"
        }
      ],
      "clips": [
        {
          "id": "clp-bass",
          "file": "clips/bass.notes",
          "start": "1.1"
        }
      ]
    }
  ]
}
)";

    const Files drumFiles {
        { "project.json", drumProject },
        { "clips/beat.notes", "@length 1bar\n\n1.1      kick  1/16  v110\n1.2      snare 1/16  v100\n1.3      kick  1/16  v110\n"
                              "1.4      snare 1/16  v100\n1.4.3    A#5   1/16  v60\n" },
        { "clips/bass.notes", "@length 1bar\n\n1.1      C2    1/4   v100\n" },
    };
}

TEST_CASE ("a drum project round-trips byte for byte, and reads without any kit present")
{
    CHECK (write (parseOrFail (drumFiles)) == drumFiles);

    const auto project = parseOrFail (drumFiles);
    REQUIRE (project.tracks.size() == 3);

    const auto& drums = project.tracks[0];
    CHECK (drums.type == TrackType::drums);
    REQUIRE (drums.devices.size() == 1);
    CHECK (drums.devices[0].type == DeviceType::drumSampler);
    CHECK (drums.devices[0].kit == "acoustic");
    CHECK (drums.drumMap.size() == 4);
    CHECK (drums.drumMap.noteOf ("hat-closed") == 42);
    CHECK (drumMapOf (drums) == &drums.drumMap);

    // The bass track has no map, and its synth no kit.
    CHECK (drumMapOf (project.tracks[2]) == nullptr);
    CHECK (project.tracks[2].devices[0].kit.empty());

    const auto& beat = notesOf (project, drums.clips[0]);
    REQUIRE (beat.notes.size() == 5);
    CHECK (beat.notes[0].pitch == 36);
    CHECK (beat.notes[1].pitch == 38);
    CHECK (beat.notes[4].pitch == 82);

    CHECK (tracksUsing (project, "clips/beat.notes") == std::vector<const Track*> { &project.tracks[0], &project.tracks[1] });
    CHECK (drumMapOfFile (project, "clips/beat.notes") == &drums.drumMap);
    CHECK (drumMapOfFile (project, "clips/bass.notes") == nullptr);
}

TEST_CASE ("a drum map is written by note, and drum clips name every note their map names")
{
    auto files = drumFiles;
    files["project.json"] = replaced (drumProject, "\"kick\": 36,\n        \"snare\": 38,\n        \"hat-closed\": 42,\n        \"crash\": 49",
                                      "\"crash\": 49, \"hat-closed\": 42, \"kick\": 36, \"snare\": 38");
    files["clips/beat.notes"] = "1.4.3 A#5 1/16 v60\n1.1 C2 1/16 v110\n1.2 snare 1/16 v100\n1.3 kick 1/16 v110\n1.4 D2 1/16 v100\n";

    CHECK (write (parseOrFail (files)) == drumFiles);
}

TEST_CASE ("a file used on several tracks must mean the same notes on each")
{
    struct Case
    {
        std::string_view from; // edit made to the drum project
        std::string_view to;
        std::string_view message;
    };

    const Case cases[] = {
        // The two maps put a name the file uses on different notes.
        { "\"snare\": 38,\n        \"cowbell\"", "\"snare\": 40,\n        \"cowbell\"",
          "track 'trk-perc', clip 'clp-perc': clips/beat.notes is also used on track 'trk-drums', but the two tracks' drum maps don't name "
          "its notes alike: note 38 (D2) is 'snare' on trk-drums but has no name on trk-perc (its 'snare' is note 40). A file used on several "
          "tracks must mean the same notes on each, so make the maps agree on these notes, or give this clip a file of its own" },

        // The maps give a note in the file different names.
        { "\"kick\": 36,\n        \"snare\": 38,\n        \"cowbell\"", "\"bd\": 36,\n        \"snare\": 38,\n        \"cowbell\"",
          "note 36 (C2) is 'kick' on trk-drums but 'bd' on trk-perc" },

        // A note the file writes as a note name, which only one map names.
        { "\"cowbell\": 56", "\"cowbell\": 82", "note 82 (A#5) is 'cowbell' on trk-perc but has no name on trk-drums" },

        // A track with no drum map at all.
        { "\"clips/bass.notes\"", "\"clips/beat.notes\"", "track 'trk-bass', clip 'clp-bass': clips/beat.notes is also used on track "
                                                          "'trk-drums', but the two tracks' drum maps don't name its notes alike: note 36 (C2) "
                                                          "is 'kick' on trk-drums but trk-bass is a \"midi\" track, with no drum map; note 38 (D2)" },
    };

    for (const auto& c : cases)
    {
        CAPTURE (c.to);
        auto files = drumFiles;
        files["project.json"] = replaced (drumProject, c.from, c.to);

        const auto result = parseFiles (files);
        const auto error = errorMatching (result, c.message);
        CHECK (error.file == "project.json");
        CHECK (result.errors.size() == 1);
    }

    // Maps that differ only on notes the file doesn't use are fine: hat-closed, crash and cowbell here.
    CHECK (parseFiles (drumFiles).project);
}

TEST_CASE ("errors in drum maps, drum samplers and drums tracks point at the value and say how to fix it")
{
    struct Case
    {
        std::string_view from; // edit made to the drum project
        std::string_view to;
        std::string_view at;   // where the error should point, in the edited text
        std::string_view message;
    };

    const Case drumCases[] = {
        { R"("kick": 36,
        "snare": 38,
        "hat-closed")", R"("Kick": 36,
        "snare": 38,
        "hat-closed")", R"("Kick")", "track 'trk-drums': \"drumMap\": 'Kick' isn't a valid drum name: drum names are lowercase letters and digits" },
        { R"("crash": 49)", R"("c2": 49)", R"("c2")", "\"drumMap\": 'c2' can't be a drum name: on a note line, it reads as the note name C2" },
        { R"("crash": 49)", R"("v1": 49)", R"("v1")", "'v1' can't be a drum name: on a note line, 'v' with a number is a velocity" },
        { R"("crash": 49)", R"("crash": "C#3")", R"("C#3")", "\"drumMap\": \"crash\" should be a MIDI note number from 0 to 127, e.g. 36, "
                                                             "not a note name: drum maps use General MIDI's numbers, and 'C#3' is 49" },
        { R"("crash": 49)", R"("crash": 128)", "128", "\"crash\" should be a MIDI note number from 0 to 127, e.g. 36" },
        { R"("crash": 49)", R"("crash": 49.5)", "49.5", "\"crash\" should be a MIDI note number from 0 to 127" },
        { R"("crash": 49)", R"("crash": 42)", R"(42
      },
      "clips": [
        {
          "id": "clp-beat")", "\"drumMap\": \"crash\" and \"hat-closed\" are both note 42, but a note has one name in a drum map" },
        { R"("crash": 49)", R"("crash": 49, "kick": 35)", R"("kick": 35)", "\"drumMap\": \"kick\" appears twice; a name stands for one note" },
        { R"("drumMap": {
        "kick": 36,
        "snare": 38,
        "hat-closed")", R"("drumMap": [ 36 ], "x": {
        "kick": 36,
        "snare": 38,
        "hat-closed")", "[ 36 ]", "track 'trk-drums': \"drumMap\" should be an object of drum names and the MIDI notes they play, "
                                  "e.g. { \"kick\": 36, \"snare\": 38, \"hat-closed\": 42 }" },
        { R"("devices": [],
      "drumMap": {
        "kick": 36,
        "snare": 38,
        "cowbell": 56
      },)", R"("devices": [],)", R"({
      "id": "trk-perc")", "track 'trk-perc': missing \"drumMap\"; add e.g. \"drumMap\": { \"kick\": 36, \"snare\": 38, \"hat-closed\": 42 }" },
        { R"("type": "drums",
      "volume": "0dB",
      "pan": "R30")", R"("type": "midi",
      "volume": "0dB",
      "pan": "R30")", R"("drumMap": {
        "kick": 36,
        "snare": 38,
        "cowbell")", "track 'trk-perc': only \"drums\" tracks have a \"drumMap\"; other tracks' clips use note names. "
                     "Make the track \"type\": \"drums\", or remove its drum map" },
        { R"("type": "hocket:synth")", R"("type": "hocket:drum-sampler", "kit": "acoustic")", R"("hocket:drum-sampler", "kit")",
          "device 'dev-bass-synth': a \"hocket:drum-sampler\" goes on a \"drums\" track, whose drum map names the kit's pieces; this is "
          "a \"midi\" track. Make it \"type\": \"drums\" with a \"drumMap\", or use \"hocket:synth\"" },
        { R"("kit": "acoustic")", R"("kit": "Acoustic Kit")", R"("Acoustic Kit")", "device 'dev-drums-kit': 'Acoustic Kit' isn't a valid kit name: "
                                                                                   "kit names are lowercase letters and digits in words joined by single hyphens" },
        { R"("kit": "acoustic")", R"("kit": 1)", R"(1
        })", "device 'dev-drums-kit': \"kit\" should be a string, e.g. \"acoustic\"" },
        { R"(,
          "kit": "acoustic")", "", R"({
          "id": "dev-drums-kit")", "device 'dev-drums-kit': missing \"kit\"; add e.g. \"kit\": \"acoustic\"" },
        { R"("type": "hocket:synth")", R"("type": "hocket:synth", "kit": "warm")", R"("kit": "warm")",
          "device 'dev-bass-synth': only a \"hocket:drum-sampler\" has a \"kit\"" },
    };

    for (const auto& c : drumCases)
    {
        CAPTURE (c.to);
        auto files = drumFiles;
        files["project.json"] = replaced (drumProject, c.from, c.to);

        const auto error = errorMatching (parseFiles (files), c.message);
        const auto expected = positionOf (files["project.json"], c.at);
        CHECK (error.file == "project.json");
        CHECK (error.line == expected.line);
        CHECK (error.column == expected.column);
    }
}

TEST_CASE ("a broken drum map stops its track's clips being read, rather than piling up errors in them")
{
    auto files = drumFiles;
    files["project.json"] = replaced (drumProject, R"("kick": 36,
        "snare": 38,
        "hat-closed")", R"("kick": "36",
        "snare": 38,
        "hat-closed")");

    // trk-drums's map is broken; trk-perc, which reads the file instead, has kick.
    const auto result = parseFiles (files);
    REQUIRE (result.errors.size() == 1);
    CHECK (contains (result.errors[0].message, "\"kick\" should be a MIDI note number"));
}

TEST_CASE ("the drum project fixture loads from disk with no kits installed, and is in canonical form")
{
    const auto folder = hocket::pathFromUtf8 (HOCKET_FIXTURES_DIR "/drum-project");
    const auto result = load (folder);

    for (const auto& error : result.errors)
        MESSAGE (formatDiagnostic (error));

    REQUIRE (result.project);
    CHECK (result.project->tracks.front().type == TrackType::drums);

    for (const auto& [path, contents] : write (*result.project))
    {
        CAPTURE (path);
        CHECK (readFile (folder / hocket::pathFromUtf8 (path)) == contents);
    }
}

TEST_CASE ("diagnostics are formatted with as much location as they have")
{
    CHECK (formatDiagnostic ({ "project.json", 4, 12, "bad" }) == "project.json:4:12: bad");
    CHECK (formatDiagnostic ({ "project.json", 0, 0, "bad" }) == "project.json: bad");
    CHECK (formatDiagnostic ({ "", 0, 0, "bad" }) == "bad");
}
