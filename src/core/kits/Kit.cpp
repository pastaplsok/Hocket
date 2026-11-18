#include "core/kits/Kit.h"

#include "core/Suggestions.h"
#include "core/notes/DrumMap.h"
#include "core/notes/NotesSyntax.h"
#include "core/project/Rules.h"
#include "core/project/StrictJson.h"
#include "core/units/Units.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>

namespace hocket::kits
{
namespace
{
    using project::jsonQuoted;
    using project::KeySpec;
    using project::singleQuoted;
    using Json = nlohmann::json;

    constexpr KeySpec manifestKeys[] = {
        { "format", R"("hocket-kit")" },
        { "formatVersion", "0" },
        { "description", R"("Electronic, 808-style")" },
        { "pieces", R"([{ "name": "kick", "note": 36, "file": "kick.wav", "level": "0dB" }])" },
        { "chokeGroups", R"([["hat-closed", "hat-open"]])" },
    };

    constexpr KeySpec pieceKeys[] = {
        { "name", R"("kick")" },
        { "note", "36" },
        { "file", R"("kick.wav")" },
        { "level", R"("0dB")" },
    };

    bool endsWithWav (std::string_view file)
    {
        constexpr std::string_view extension = ".wav";

        if (file.size() <= extension.size())
            return false;

        const auto tail = file.substr (file.size() - extension.size());
        return std::ranges::equal (tail, extension, [] (char a, char b) { return std::tolower (static_cast<unsigned char> (a)) == b; });
    }

    class ManifestReader : private project::StrictJson
    {
    public:
        explicit ManifestReader (std::string_view text) : StrictJson (text, std::string (manifestFileName)) {}

        ManifestResult run()
        {
            if (const auto root = parse())
                readManifest (*root);

            std::ranges::stable_sort (errors, [] (const project::Diagnostic& a, const project::Diagnostic& b)
            {
                return std::tie (a.line, a.column) < std::tie (b.line, b.column);
            });

            if (! errors.empty())
                return { {}, std::move (errors) };

            return { std::move (manifest), {} };
        }

    private:
        Manifest manifest;

        void readManifest (const Json& root)
        {
            const std::string where = "the kit";

            if (! requireObject (root, where, manifestKeys))
                return;

            checkKeys (root, where, manifestKeys);
            checkFormat (root, where, manifestKeys, "hocket-kit", formatVersion, "kit manifest");
            collectPieceNames (root);

            if (const auto description = readString (root, "description", where, manifestKeys))
            {
                if (project::isOneLineText (*description, maxDescriptionLength))
                    manifest.description = *description;
                else
                    errorAt (root.at ("description"), where + ": \"description\" should be one line of text, 1 to "
                                                          + std::to_string (maxDescriptionLength) + " bytes, saying what the kit is, e.g. "
                                                          + std::string (project::exampleFor (manifestKeys, "description")));
            }

            if (const auto* pieces = readArray (root, "pieces", where, manifestKeys))
                readPieces (*pieces, where);

            if (const auto* groups = readArray (root, "chokeGroups", where, manifestKeys))
                readChokeGroups (*groups, where);
        }

        void readPieces (const Json& pieces, const std::string& kitWhere)
        {
            if (pieces.empty())
            {
                errorAt (pieces, kitWhere + ": \"pieces\" is empty; a kit needs at least one, e.g. "
                                     + std::string (project::exampleFor (manifestKeys, "pieces")));
                return;
            }

            std::map<std::string, std::size_t> names; // name -> line it was first used on
            std::map<int, std::pair<std::string, int>> notes; // note -> name, line

            for (std::size_t i = 0; i < pieces.size(); ++i)
            {
                const auto& value = pieces[i];
                const auto fallback = "pieces[" + std::to_string (i) + "]";

                if (! requireObject (value, fallback, pieceKeys))
                    continue;

                Piece piece;
                auto ok = true;
                const auto name = readString (value, "name", fallback, pieceKeys);
                auto where = fallback;

                if (! name)
                {
                    ok = false;
                }
                else if (auto problem = notes::drumNameProblem (*name))
                {
                    errorAt (value.at ("name"), fallback + ": \"name\": " + *problem);
                    ok = false;
                }
                else if (const auto [first, inserted] = names.try_emplace (*name, static_cast<std::size_t> (lineOf (value.at ("name")))); ! inserted)
                {
                    errorAt (value.at ("name"), fallback + ": " + singleQuoted (*name) + " is already a piece, on line " + std::to_string (first->second)
                                                    + "; each piece has a name of its own");
                    ok = false;
                }
                else
                {
                    piece.name = *name;
                    where = "piece " + singleQuoted (*name);
                }

                checkKeys (value, where, pieceKeys);

                if (const auto* note = member (value, "note"))
                {
                    if (! note->is_number_integer() || note->get<std::int64_t>() < 0 || note->get<std::int64_t>() > 127)
                    {
                        auto message = where + ": \"note\" should be a MIDI note number from 0 to 127, e.g. 36";

                        if (note->is_string())
                            if (const auto pitch = notes::parsePitch (note->get<std::string>()); pitch.value)
                                message += ", not a note name: kits use General MIDI's numbers, and " + singleQuoted (note->get<std::string>()) + " is "
                                         + std::to_string (pitch.value->pitch);

                        errorAt (*note, std::move (message));
                        ok = false;
                    }
                    else
                    {
                        piece.note = static_cast<int> (note->get<std::int64_t>());
                        const auto [other, inserted] = notes.try_emplace (piece.note, name.value_or (""), lineOf (*note));

                        if (! inserted)
                        {
                            const auto& [otherName, otherLine] = other->second;
                            errorAt (*note, where + ": note " + std::to_string (piece.note) + " is already "
                                                + (otherName.empty() ? std::string ("another piece's") : "piece " + singleQuoted (otherName) + "'s")
                                                + ", on line " + std::to_string (otherLine) + "; a note plays one piece, so give this one another note");
                            ok = false;
                        }
                    }
                }
                else
                {
                    ok = false;
                }

                if (const auto file = readString (value, "file", where, pieceKeys))
                {
                    if (const auto problem = sampleFileProblem (*file))
                    {
                        errorAt (value.at ("file"), where + ": \"file\": " + *problem);
                        ok = false;
                    }
                    else
                    {
                        piece.file = *file;
                    }
                }
                else
                {
                    ok = false;
                }

                if (const auto level = readString (value, "level", where, pieceKeys))
                {
                    const auto parsed = units::parse (*level, units::volume());

                    if (! parsed.value)
                    {
                        errorAt (value.at ("level"), where + ": \"level\": " + parsed.error);
                        ok = false;
                    }
                    else if (std::isinf (*parsed.value))
                    {
                        errorAt (value.at ("level"), where + ": \"level\" is -infdB, so the piece would be silent; remove it from the kit instead");
                        ok = false;
                    }
                    else
                    {
                        piece.level = *parsed.value;
                    }
                }
                else
                {
                    ok = false;
                }

                if (ok)
                    manifest.pieces.push_back (std::move (piece));
            }
        }

        // [["hat-closed", "hat-pedal", "hat-open"], ["kick"]]
        void readChokeGroups (const Json& groups, const std::string& kitWhere)
        {
            const auto example = std::string (project::exampleFor (manifestKeys, "chokeGroups"));
            std::map<std::string, std::size_t> grouped; // piece -> its group's index

            for (std::size_t i = 0; i < groups.size(); ++i)
            {
                const auto& group = groups[i];
                const auto where = kitWhere + ": \"chokeGroups\"[" + std::to_string (i) + "]";

                if (! group.is_array() || group.empty())
                {
                    errorAt (group, where + " should be a list of the pieces that cut each other off, e.g. [\"hat-closed\", \"hat-open\"]; "
                                            "a piece alone in a group cuts only itself");
                    continue;
                }

                std::vector<std::string> members;

                for (const auto& member : group)
                {
                    if (! member.is_string())
                    {
                        errorAt (member, where + " should name pieces, as strings, e.g. " + example);
                        continue;
                    }

                    const auto name = member.get<std::string>();

                    if (! std::ranges::any_of (pieceNamesWritten, [&name] (const std::string& piece) { return piece == name; }))
                    {
                        errorAt (member, where + ": there's no piece " + singleQuoted (name) + suggestion (name));
                        continue;
                    }

                    if (const auto [other, inserted] = grouped.try_emplace (name, i); ! inserted)
                    {
                        errorAt (member, where + ": " + singleQuoted (name)
                                             + (other->second == i ? " appears twice in this group; keep only one"
                                                                   : " is already in choke group " + std::to_string (other->second)
                                                                         + "; a piece is in one group at most, so merge the two groups, or take it out of one"));
                        continue;
                    }

                    members.push_back (name);
                }

                manifest.chokeGroups.push_back (std::move (members));
            }
        }

        // The pieces' names as written, valid or not, so a choke group naming
        // a piece with another error isn't reported as naming no piece.
        std::vector<std::string> pieceNamesWritten;

        std::string suggestion (std::string_view name) const
        {
            std::string nearest;
            std::size_t distance = 3;

            for (const auto& piece : pieceNamesWritten)
                if (const auto d = editDistance (name, piece); d < distance)
                    distance = d, nearest = piece;

            if (! nearest.empty())
                return " (did you mean " + singleQuoted (nearest) + "?)";

            std::string list;

            for (const auto& piece : pieceNamesWritten)
                list += (list.empty() ? "" : ", ") + singleQuoted (piece);

            return list.empty() ? std::string() : "; the pieces are " + list;
        }

        void collectPieceNames (const Json& root)
        {
            if (const auto* pieces = root.is_object() ? member (root, "pieces") : nullptr; pieces != nullptr && pieces->is_array())
                for (const auto& piece : *pieces)
                    if (const auto* name = piece.is_object() ? member (piece, "name") : nullptr; name != nullptr && name->is_string())
                        pieceNamesWritten.push_back (name->get<std::string>());
        }
    };
}

//==============================================================================
const Piece* Manifest::pieceOn (int note) const
{
    const auto found = std::ranges::find (pieces, note, &Piece::note);
    return found == pieces.end() ? nullptr : &*found;
}

const Piece* Manifest::pieceNamed (std::string_view name) const
{
    const auto found = std::ranges::find (pieces, name, &Piece::name);
    return found == pieces.end() ? nullptr : &*found;
}

int Manifest::chokeGroupOf (std::string_view piece) const
{
    for (std::size_t i = 0; i < chokeGroups.size(); ++i)
        if (std::ranges::find (chokeGroups[i], piece) != chokeGroups[i].end())
            return static_cast<int> (i);

    return -1;
}

ManifestResult parseManifest (std::string_view text)
{
    return ManifestReader (text).run();
}

std::optional<std::string> sampleFileProblem (std::string_view file)
{
    const auto example = " e.g. \"kick.wav\" or \"hats/closed.wav\"";

    if (! project::isOneLineText (file, maxSampleFileLength))
        return "a sample's file should be one line of text, 1 to " + std::to_string (maxSampleFileLength) + " bytes,"
             + example;

    if (file.find ('\\') != std::string_view::npos)
        return singleQuoted (file) + " has a backslash; write folders with forward slashes," + example;

    if (file.starts_with ('/') || file.find (':') != std::string_view::npos)
        return singleQuoted (file) + " isn't inside the kit's folder: a sample's file is relative to the folder kit.json is in," + example;

    for (std::size_t start = 0; start <= file.size();)
    {
        const auto end = std::min (file.find ('/', start), file.size());
        const auto part = file.substr (start, end - start);

        if (part.empty() || part == "." || part == "..")
            return singleQuoted (file) + " should name a file inside the kit's folder, with no empty, \".\" or \"..\" parts in its path," + example;

        start = end + 1;
    }

    if (file.find_first_of ("<>\"|?*") != std::string_view::npos)
        return singleQuoted (file) + " has a character Windows doesn't allow in a file name (one of < > \" | ? *)";

    if (! endsWithWav (file))
        return singleQuoted (file) + " isn't a .wav file; kits' samples are WAV files, 16- or 24-bit, mono or stereo," + example;

    return {};
}
}
