#include "core/notes/NotesPatch.h"
#include "core/notes/NotesSyntax.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace hocket::notes
{
namespace
{
    std::string trimmed (std::string_view text)
    {
        while (! text.empty() && (text.front() == ' ' || text.front() == '\t'))
            text.remove_prefix (1);

        while (! text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
            text.remove_suffix (1);

        return std::string (text);
    }

    std::string countText (std::size_t count, std::string_view noun)
    {
        return std::to_string (count) + " " + std::string (noun) + (count == 1 ? "" : "s");
    }

    std::string join (const std::vector<std::string>& parts, std::string_view separator)
    {
        std::string text;

        for (const auto& part : parts)
            text += (text.empty() ? "" : std::string (separator)) + part;

        return text;
    }

    // "kick", or "Eb2"
    std::string pitchText (const Note& note, const DrumMap* drumMap)
    {
        if (drumMap != nullptr)
            if (const auto* name = drumMap->nameOf (note.pitch))
                return *name;

        return formatPitch (note.pitch, note.spelling);
    }

    // The lines these notes are written as in canonical form, as read_clip shows them.
    std::string linesOf (const Clip& clip, std::vector<Note> notes, Meter meter, const DrumMap* drumMap)
    {
        Clip part;
        part.lengthInBars = clip.lengthInBars;
        part.notes = std::move (notes);

        auto text = write (part, meter, drumMap);
        const auto start = text.find ("\n\n"); // after the @length line
        text = start == std::string::npos ? std::string() : text.substr (start + 2);

        while (! text.empty() && text.back() == '\n')
            text.pop_back();

        return text;
    }

    // The notes written on one line with `note`: the same start, length and velocity.
    std::vector<std::size_t> lineOf (const Clip& clip, const Note& note)
    {
        std::vector<std::size_t> line;

        for (std::size_t i = 0; i < clip.notes.size(); ++i)
        {
            const auto& other = clip.notes[i];

            if (other.start == note.start && other.length == note.length && other.velocity == note.velocity)
                line.push_back (i);
        }

        return line;
    }

    std::vector<Note> notesAt (const Clip& clip, const std::vector<std::size_t>& indices)
    {
        std::vector<Note> notes;

        for (const auto i : indices)
            notes.push_back (clip.notes[i]);

        return notes;
    }

    // A line's comments as the writer merges them: leading ones in order, trailing ones joined with "; ".
    Comments mergedComments (const Clip& clip, const std::vector<std::size_t>& line)
    {
        Comments merged;
        std::vector<std::string> trailing;

        for (const auto i : line)
        {
            const auto& comments = clip.notes[i].comments;
            merged.leading.insert (merged.leading.end(), comments.leading.begin(), comments.leading.end());

            if (comments.trailing && ! trimmed (*comments.trailing).empty())
                trailing.push_back (trimmed (*comments.trailing));
        }

        if (! trailing.empty())
            merged.trailing = " " + join (trailing, "; ");

        return merged;
    }

    // An edit's old or new text, read as lines of this clip.
    struct Lines
    {
        std::optional<Clip> clip;
        std::string error;
    };

    Lines readLines (std::string_view text, const std::string& which, const Clip& clip, Meter meter, const DrumMap* drumMap)
    {
        for (std::size_t start = 0; start <= text.size();)
        {
            const auto end = std::min (text.find ('\n', start), text.size());

            if (trimmed (text.substr (start, end - start)).starts_with ("@"))
                return { {}, which + " has a directive line, but patch_clip changes notes only: give a new @length with write_clip" };

            start = end + 1;
        }

        // Read under the clip's own @length, so a note at or past its end is an
        // error, and comments before the first note go with it, not a header.
        const auto bars = std::to_string (clip.lengthInBars) + (clip.lengthInBars == 1 ? "bar" : "bars");
        auto parsed = parse ("@length " + bars + "\n" + std::string (text), meter, drumMap);

        if (! parsed.clip)
        {
            std::vector<std::string> problems;

            for (std::size_t i = 0; i < parsed.errors.size() && i < 3; ++i)
                problems.push_back ("line " + std::to_string (parsed.errors[i].line - 1) + ":" + std::to_string (parsed.errors[i].column) + ": "
                                    + parsed.errors[i].message);

            if (parsed.errors.size() > 3)
                problems.push_back ("and " + countText (parsed.errors.size() - 3, "more problem"));

            return { {}, which + " can't be read: " + join (problems, "; ") };
        }

        if (parsed.clip->notes.empty())
            return { {}, which + " has no notes: give its lines in .notes form, e.g. \"1.3 E2 1/8 v90\"" };

        return { std::move (parsed.clip), {} };
    }

    // "1.3 E2", "2.2 snare"
    std::string noteName (const Note& note, Meter meter, const DrumMap* drumMap)
    {
        return formatPosition (note.start, meter) + " " + pitchText (note, drumMap);
    }

    std::optional<std::size_t> find (const Clip& clip, const Note& note)
    {
        for (std::size_t i = 0; i < clip.notes.size(); ++i)
            if (clip.notes[i].start == note.start && clip.notes[i].pitch == note.pitch)
                return i;

        return std::nullopt;
    }

    // Takes a note out, leaving its line's comments with what's left of the line.
    void removeNote (Clip& clip, std::size_t index)
    {
        const auto line = lineOf (clip, clip.notes[index]);
        auto merged = mergedComments (clip, line);

        for (const auto i : line)
            clip.notes[i].comments = {};

        const auto first = std::ranges::find_if (line, [index] (std::size_t i) { return i != index; });

        if (first != line.end())
            clip.notes[*first].comments = std::move (merged);

        clip.notes.erase (clip.notes.begin() + static_cast<std::ptrdiff_t> (index));
    }
}

PatchResult patch (const Clip& clip, std::span<const PatchEdit> edits, Meter meter, const DrumMap* drumMap)
{
    auto working = clip;
    std::vector<std::string> summaries;

    for (std::size_t e = 0; e < edits.size(); ++e)
    {
        const auto& edit = edits[e];
        const auto label = "edit " + std::to_string (e + 1);
        const auto fail = [&label] (const std::string& why) { return PatchResult { {}, label + why + "; nothing was changed", {} }; };
        const auto failShowing = [&label] (const std::string& why, const std::string& lines)
        { return PatchResult { {}, label + why + ", so nothing was changed. " + lines, {} }; };

        if (! edit.oldText && ! edit.newText)
            return fail (" has neither old nor new: give old to remove notes, new to add them, or both to replace them");

        std::size_t removed = 0;
        std::size_t added = 0;

        if (edit.oldText)
        {
            const auto old = readLines (*edit.oldText, "'s old", working, meter, drumMap);

            if (! old.clip)
                return fail (old.error);

            for (const auto& wanted : old.clip->notes)
            {
                const auto name = noteName (wanted, meter, drumMap);
                const auto index = find (working, wanted);

                if (! index)
                {
                    const auto bar = wanted.start / meter.ticksPerBar();
                    std::vector<Note> inBar;

                    for (const auto& note : working.notes)
                        if (note.start / meter.ticksPerBar() == bar)
                            inBar.push_back (note);

                    const auto barText = "bar " + std::to_string (bar + 1);
                    return failShowing (": " + name + " isn't in the clip",
                                        inBar.empty() ? "There are no notes in " + barText + " now."
                                                      : "Its lines in " + barText + " now:\n" + linesOf (working, std::move (inBar), meter, drumMap));
                }

                const auto& stored = working.notes[*index];
                const auto line = lineOf (working, stored);
                std::vector<std::string> differences;

                if (stored.length != wanted.length)
                    differences.push_back ("length " + formatLength (stored.length) + ", not " + formatLength (wanted.length));

                if (stored.velocity != wanted.velocity)
                    differences.push_back ("velocity v" + std::to_string (stored.velocity) + ", not v" + std::to_string (wanted.velocity));

                if (wanted.comments.trailing && ! trimmed (*wanted.comments.trailing).empty())
                {
                    const auto storedComment = mergedComments (working, line).trailing;
                    const auto storedText = storedComment ? trimmed (*storedComment) : std::string();
                    const auto wantedText = trimmed (*wanted.comments.trailing);

                    if (storedText != wantedText)
                        differences.push_back ((storedText.empty() ? std::string ("no comment") : "the comment \"#" + std::string (*storedComment) + "\"")
                                               + ", not \"#" + *wanted.comments.trailing + "\"");
                }

                if (! differences.empty())
                    return failShowing (": " + name + " is in the clip, but with " + join (differences, " and "),
                                        "Its line is:\n" + linesOf (working, notesAt (working, line), meter, drumMap));

                removeNote (working, *index);
                ++removed;
            }
        }

        if (edit.newText)
        {
            const auto fresh = readLines (*edit.newText, "'s new", working, meter, drumMap);

            if (! fresh.clip)
                return fail (fresh.error);

            for (const auto& note : fresh.clip->notes)
            {
                if (const auto index = find (working, note))
                    return failShowing (": " + noteName (note, meter, drumMap) + " is in the clip already",
                                        "Its line is:\n" + linesOf (working, notesAt (working, lineOf (working, working.notes[*index])), meter, drumMap)
                                            + "\nTo change it, put it in old as it is, and in new as it should be.");

                working.notes.push_back (note);
                ++added;
            }

            // Comments after new's last note come after the clip's last line, as write_clip keeps them.
            working.footer.insert (working.footer.end(), fresh.clip->footer.begin(), fresh.clip->footer.end());
        }

        if (removed > 0 && added > 0)
            summaries.push_back (label + " removed " + countText (removed, "note") + " and added " + std::to_string (added));
        else if (removed > 0)
            summaries.push_back (label + " removed " + countText (removed, "note"));
        else
            summaries.push_back (label + " added " + countText (added, "note"));
    }

    return { std::move (working), {}, join (summaries, "; ") };
}
}
