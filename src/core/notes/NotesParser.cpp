#include "core/notes/Notes.h"
#include "core/notes/NotesSyntax.h"

#include <algorithm>
#include <map>
#include <utility>

namespace hocket::notes
{
namespace
{
    constexpr std::size_t maxErrors = 20;
    constexpr const char* noteLineExample = "'1.1  C2  1/8  v96'";

    struct Field
    {
        std::string_view text;
        int column = 0;
    };

    bool isSpace (char c)
    {
        return c == ' ' || c == '\t';
    }

    std::string_view trimRight (std::string_view text)
    {
        while (! text.empty() && isSpace (text.back()))
            text.remove_suffix (1);

        return text;
    }

    std::string_view trimLeft (std::string_view text)
    {
        while (! text.empty() && isSpace (text.front()))
            text.remove_prefix (1);

        return text;
    }

    // '#' starts a comment only at the start of a line or after whitespace, so
    // the '#' in F#4 isn't one.
    std::size_t findComment (std::string_view line)
    {
        for (std::size_t i = 0; i < line.size(); ++i)
            if (line[i] == '#' && (i == 0 || isSpace (line[i - 1])))
                return i;

        return std::string_view::npos;
    }

    std::string quoted (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }

    bool isDigits (std::string_view text)
    {
        return ! text.empty() && std::ranges::all_of (text, [] (char c) { return c >= '0' && c <= '9'; });
    }

    // "cc74", "pb", "at" (channel aftertouch): SPEC §5
    bool looksLikeController (std::string_view text)
    {
        return text == "pb" || text == "at" || (text.starts_with ("cc") && isDigits (text.substr (2)));
    }

    // e.g. "c2", "f#4", "bb3": a note name with a lowercase letter
    bool looksLikeLowercaseNote (std::string_view text)
    {
        if (text.empty() || text[0] < 'a' || text[0] > 'g')
            return false;

        auto rest = text.substr (1);

        if (rest.starts_with ('#') || rest.starts_with ('b'))
            rest.remove_prefix (1);

        if (rest.starts_with ('-'))
            rest.remove_prefix (1);

        return isDigits (rest);
    }

    class Parser
    {
    public:
        explicit Parser (Meter meterToUse) : meter (meterToUse) {}

        ParseResult run (std::string_view text)
        {
            if (text.starts_with ("\xEF\xBB\xBF"))
                text.remove_prefix (3);

            int lineNumber = 0;

            for (std::size_t start = 0; start <= text.size();)
            {
                const auto newline = text.find ('\n', start);
                auto line = text.substr (start, newline == std::string_view::npos ? std::string_view::npos : newline - start);

                if (line.ends_with ('\r'))
                    line.remove_suffix (1);

                parseLine (line, ++lineNumber);

                if (newline == std::string_view::npos)
                    break;

                start = newline + 1;
            }

            clip.footer = std::move (pendingComments);
            finish();

            std::ranges::stable_sort (errors, [] (const Diagnostic& a, const Diagnostic& b)
            {
                return std::pair (a.line, a.column) < std::pair (b.line, b.column);
            });

            if (errors.size() > maxErrors)
            {
                const auto line = errors[maxErrors].line;
                errors.resize (maxErrors);
                errors.push_back ({ line, 1, "stopping after " + std::to_string (maxErrors) + " errors; fix these first" });
            }

            if (! errors.empty())
                return { {}, std::move (errors) };

            return { std::move (clip), {} };
        }

    private:
        struct NoteSource
        {
            int line = 0;
            int positionColumn = 0;
            int pitchColumn = 0;
        };

        Meter meter;
        Clip clip;
        std::vector<Diagnostic> errors;
        std::vector<std::string> pendingComments; // full-line comments waiting for the next line
        bool seenContent = false;
        std::optional<int> lengthLine;
        std::vector<NoteSource> noteSources;      // where each of clip.notes was written

        void error (int line, int column, std::string message)
        {
            errors.push_back ({ line, column, std::move (message) });
        }

        static int endColumn (const Field& field)
        {
            return field.column + static_cast<int> (field.text.size());
        }

        void parseLine (std::string_view line, int lineNumber)
        {
            std::optional<std::string> comment;
            auto content = line;

            if (const auto hash = findComment (line); hash != std::string_view::npos)
            {
                comment = std::string (trimRight (line.substr (hash + 1)));
                content = line.substr (0, hash);
            }

            content = trimRight (content);

            if (trimLeft (content).empty())
            {
                if (comment)
                    (seenContent ? pendingComments : clip.header).push_back (std::move (*comment));

                return;
            }

            seenContent = true;
            Comments comments { std::exchange (pendingComments, {}), std::move (comment) };

            std::vector<Field> fields;

            if (! splitFields (content, lineNumber, fields))
                return;

            if (fields[0].text.starts_with ('@'))
                parseDirective (fields, std::move (comments), lineNumber);
            else
                parseNoteLine (fields, std::move (comments), lineNumber);
        }

        // Whitespace-separated fields; a [...] chord is one field.
        bool splitFields (std::string_view content, int lineNumber, std::vector<Field>& fields)
        {
            for (std::size_t i = 0; i < content.size();)
            {
                if (isSpace (content[i]))
                {
                    ++i;
                    continue;
                }

                const auto start = i;

                if (content[i] == '[')
                {
                    const auto close = content.find (']', i);

                    if (close == std::string_view::npos)
                    {
                        error (lineNumber, static_cast<int> (start) + 1,
                               "this chord has no closing ']'; chords look like '[C3 E3 G3]'");
                        return false;
                    }

                    i = close + 1;
                }
                else
                {
                    while (i < content.size() && ! isSpace (content[i]))
                        ++i;
                }

                fields.push_back ({ content.substr (start, i - start), static_cast<int> (start) + 1 });
            }

            return true;
        }

        void parseDirective (const std::vector<Field>& fields, Comments comments, int lineNumber)
        {
            const auto& name = fields[0];

            if (name.text == "@swing" || name.text == "@ch")
                return error (lineNumber, name.column, quoted (name.text) + " isn't supported yet in Milestone 0; remove the line");

            if (name.text != "@length")
                return error (lineNumber, name.column, "unknown directive " + quoted (name.text)
                                                           + "; Milestone 0 supports only '@length', e.g. '@length 4bars'");

            if (lengthLine)
                return error (lineNumber, name.column, "@length is set twice; it was already set on line " + std::to_string (*lengthLine));

            if (fields.size() < 2)
                return error (lineNumber, endColumn (name), "@length needs a value, e.g. '@length 4bars'");

            if (fields.size() > 2)
                return error (lineNumber, fields[2].column, "unexpected " + quoted (fields[2].text) + " after the @length value");

            const auto& value = fields[1];
            auto number = value.text;

            if (number.ends_with ("bars"))
                number.remove_suffix (4);
            else if (number.ends_with ("bar"))
                number.remove_suffix (3);
            else
                number = {};

            const auto bars = isDigits (number) && number.size() <= 6 ? std::stoi (std::string (number)) : 0;

            if (isDigits (number) && (number.size() > 6 || bars > maxLengthInBars))
                return error (lineNumber, value.column, quoted (value.text) + " is too long: a clip is at most "
                                                            + std::to_string (maxLengthInBars) + " bars, e.g. '@length 16bars'");

            if (bars < 1)
                return error (lineNumber, value.column, quoted (value.text) + " isn't a valid @length: Milestone 0 clip lengths are "
                                                            "a whole number of bars, e.g. '@length 4bars' or '@length 1bar'");

            clip.lengthInBars = bars;
            clip.lengthComments = std::move (comments);
            lengthLine = lineNumber;
        }

        void parseNoteLine (const std::vector<Field>& fields, Comments comments, int lineNumber)
        {
            const auto& positionField = fields[0];
            const auto position = parsePosition (positionField.text, meter);

            if (! position.value)
                return error (lineNumber, positionField.column, position.error);

            if (fields.size() < 2)
                return error (lineNumber, endColumn (positionField),
                              std::string ("expected a pitch after the position; a note line is position, pitch, length, velocity, e.g. ")
                                  + noteLineExample);

            std::vector<std::pair<SpelledPitch, int>> pitches; // with their columns

            if (! parsePitchField (fields[1], lineNumber, pitches))
                return;

            if (fields.size() < 3)
                return error (lineNumber, endColumn (fields[1]), "expected a length after the pitch, e.g. '1/8'");

            const auto length = parseLength (fields[2].text);

            if (! length.value)
                return error (lineNumber, fields[2].column, length.error);

            if (fields.size() < 4)
                return error (lineNumber, endColumn (fields[2]), "expected a velocity after the length, e.g. 'v96' (v1-v127)");

            const auto velocity = parseVelocity (fields[3].text);

            if (! velocity.value)
                return error (lineNumber, fields[3].column, velocity.error);

            if (fields.size() > 4)
            {
                const auto& extra = fields[4];

                if (extra.text.starts_with ("ch") && isDigits (extra.text.substr (2)))
                    return error (lineNumber, extra.column, "per-note MIDI channels (" + quoted (extra.text)
                                                                + ") aren't supported yet in Milestone 0; remove it");

                return error (lineNumber, extra.column, "unexpected " + quoted (extra.text)
                                                            + " after the velocity; a note line is position, pitch, length, velocity, e.g. "
                                                            + noteLineExample);
            }

            for (std::size_t i = 0; i < pitches.size(); ++i)
            {
                Note note;
                note.start = *position.value;
                note.length = *length.value;
                note.pitch = pitches[i].first.pitch;
                note.velocity = *velocity.value;
                note.spelling = pitches[i].first.spelling;

                // A line's comments belong to its first note.
                if (i == 0)
                    note.comments = std::move (comments);

                clip.notes.push_back (std::move (note));
                noteSources.push_back ({ lineNumber, positionField.column, pitches[i].second });
            }
        }

        bool parsePitchField (const Field& field, int lineNumber, std::vector<std::pair<SpelledPitch, int>>& pitches)
        {
            if (! field.text.starts_with ('['))
                return parseOnePitch (field, lineNumber, pitches);

            const auto inner = field.text.substr (1, field.text.size() - 2);
            std::vector<Field> members;

            for (std::size_t i = 0; i < inner.size();)
            {
                if (isSpace (inner[i]))
                {
                    ++i;
                    continue;
                }

                const auto start = i;

                while (i < inner.size() && ! isSpace (inner[i]))
                    ++i;

                members.push_back ({ inner.substr (start, i - start), field.column + 1 + static_cast<int> (start) });
            }

            if (members.empty())
            {
                error (lineNumber, field.column, "this chord is empty; chords look like '[C3 E3 G3]'");
                return false;
            }

            bool ok = true;

            for (const auto& member : members)
                ok = parseOnePitch (member, lineNumber, pitches) && ok;

            return ok;
        }

        bool parseOnePitch (const Field& field, int lineNumber, std::vector<std::pair<SpelledPitch, int>>& pitches)
        {
            if (looksLikeController (field.text))
            {
                error (lineNumber, field.column, std::string ("controller lines ('cc74', 'pb', 'at') aren't supported yet in Milestone 0; only note lines like ")
                                                     + noteLineExample);
                return false;
            }

            if (looksLikeLowercaseNote (field.text))
            {
                error (lineNumber, field.column, quoted (field.text) + " isn't a note name: note names start with an uppercase letter, e.g. 'C2'. "
                                                                         "Lowercase names are drum-map names, which Milestone 0 doesn't support yet");
                return false;
            }

            if (! field.text.empty() && field.text[0] >= 'a' && field.text[0] <= 'z')
            {
                error (lineNumber, field.column, quoted (field.text) + " looks like a drum-map name; drum maps aren't supported yet in Milestone 0, "
                                                                         "so use a note name such as 'C1'");
                return false;
            }

            const auto pitch = parsePitch (field.text);

            if (! pitch.value)
            {
                error (lineNumber, field.column, pitch.error);
                return false;
            }

            pitches.push_back ({ *pitch.value, field.column });
            return true;
        }

        // Checks that need the whole file: the clip length and duplicate notes.
        void finish()
        {
            const auto ticksPerBar = meter.ticksPerBar();

            if (! lengthLine)
            {
                Ticks lastStart = 0;

                for (const auto& note : clip.notes)
                    lastStart = std::max (lastStart, note.start);

                // Notes past the longest clip are reported below.
                clip.lengthInBars = static_cast<int> (std::min<Ticks> (lastStart / ticksPerBar + 1, maxLengthInBars));
            }

            const auto clipEnd = clip.lengthInBars * ticksPerBar;
            std::map<std::pair<Ticks, int>, std::size_t> firstNoteAt; // (start, pitch) -> note index

            for (std::size_t i = 0; i < clip.notes.size(); ++i)
            {
                const auto& note = clip.notes[i];
                const auto& source = noteSources[i];
                const auto name = formatPitch (note.pitch, note.spelling);
                const auto position = formatPosition (note.start, meter);

                if (note.start >= maxLengthInBars * ticksPerBar)
                    error (source.line, source.positionColumn,
                           name + " at " + position + " is past bar " + std::to_string (maxLengthInBars) + ": a clip is at most "
                               + std::to_string (maxLengthInBars) + " bars long, so move the note");
                else if (note.start >= clipEnd)
                    error (source.line, source.positionColumn,
                           name + " at " + position + " starts at or after the end of the clip (@length "
                               + std::to_string (clip.lengthInBars) + (clip.lengthInBars == 1 ? "bar" : "bars")
                               + " ends at " + formatPosition (clipEnd, meter) + "); make @length longer or move the note");

                const auto [existing, inserted] = firstNoteAt.try_emplace ({ note.start, note.pitch }, i);

                if (! inserted)
                    error (source.line, source.pitchColumn,
                           "duplicate note: " + name + " at " + position + " is already on line "
                               + std::to_string (noteSources[existing->second].line) + "; remove one of them");
            }
        }
    };
}

ParseResult parse (std::string_view text, Meter meter)
{
    return Parser (meter).run (text);
}

std::string formatDiagnostic (const Diagnostic& diagnostic, std::string_view sourceName)
{
    return std::string (sourceName) + ":" + std::to_string (diagnostic.line) + ":"
         + std::to_string (diagnostic.column) + ": " + diagnostic.message;
}
}
