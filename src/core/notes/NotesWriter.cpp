#include "core/notes/Notes.h"
#include "core/notes/NotesSyntax.h"

#include <algorithm>
#include <cassert>
#include <numeric>
#include <tuple>

namespace hocket::notes
{
namespace
{
    // Fixed column widths (SPEC §5.4), so editing one line never reformats another.
    constexpr int positionWidth = 9;
    constexpr int pitchWidth = 6;
    constexpr int lengthWidth = 6;

    // One output line: a single note, or a chord of notes sharing start, length and velocity.
    struct Line
    {
        Ticks start = 0;
        Ticks length = 0;
        int velocity = 0;
        std::vector<const Note*> notes; // low to high
        Comments comments;              // merged from the notes, in file order
    };

    void appendField (std::string& out, std::string_view field, int width)
    {
        const auto size = static_cast<int> (field.size());
        out += field;
        out.append (static_cast<std::size_t> (size < width ? width - size : 2), ' ');
    }

    // Comments from the parser are already clean, but ones set through the API
    // might hold line breaks or trailing whitespace, which would break the format.
    std::string cleanComment (std::string text)
    {
        std::ranges::replace_if (text, [] (char c) { return c == '\n' || c == '\r'; }, ' ');

        while (! text.empty() && (text.back() == ' ' || text.back() == '\t'))
            text.pop_back();

        return text;
    }

    void appendCommentLines (std::string& out, const std::vector<std::string>& comments)
    {
        for (const auto& text : comments)
        {
            out += '#';
            out += cleanComment (text);
            out += '\n';
        }
    }

    void appendTrailingComment (std::string& out, const std::optional<std::string>& text)
    {
        if (text)
        {
            out += "  #";
            out += cleanComment (*text);
        }
    }

    // Merging lines into a chord keeps every comment: leading ones in order,
    // trailing ones joined with "; ".
    void mergeComments (Comments& merged, const Comments& from)
    {
        merged.leading.insert (merged.leading.end(), from.leading.begin(), from.leading.end());

        if (! from.trailing)
            return;

        if (! merged.trailing)
        {
            merged.trailing = from.trailing;
            return;
        }

        auto next = std::string_view (*from.trailing);

        while (! next.empty() && (next.front() == ' ' || next.front() == '\t'))
            next.remove_prefix (1);

        if (next.empty())
            return;

        *merged.trailing += "; ";
        *merged.trailing += next;
    }

    std::vector<Line> buildLines (const std::vector<Note>& notes)
    {
        std::vector<std::size_t> order (notes.size());
        std::iota (order.begin(), order.end(), std::size_t { 0 });

        // Notes that can share a chord line end up next to each other, low to high.
        std::ranges::sort (order, [&notes] (std::size_t a, std::size_t b)
        {
            const auto& x = notes[a];
            const auto& y = notes[b];
            return std::tie (x.start, x.length, x.velocity, x.pitch, a) < std::tie (y.start, y.length, y.velocity, y.pitch, b);
        });

        std::vector<Line> lines;

        for (std::size_t i = 0; i < order.size();)
        {
            const auto& first = notes[order[i]];
            Line line { first.start, first.length, first.velocity, {}, {} };
            std::vector<std::size_t> members;

            for (; i < order.size(); ++i)
            {
                const auto& note = notes[order[i]];

                if (note.start != line.start || note.length != line.length || note.velocity != line.velocity)
                    break;

                line.notes.push_back (&note);
                members.push_back (order[i]);
            }

            std::ranges::sort (members);

            for (const auto index : members)
                mergeComments (line.comments, notes[index].comments);

            lines.push_back (std::move (line));
        }

        std::ranges::stable_sort (lines, [] (const Line& a, const Line& b)
        {
            return std::tuple (a.start, a.notes.front()->pitch, a.length, a.velocity)
                 < std::tuple (b.start, b.notes.front()->pitch, b.length, b.velocity);
        });

        return lines;
    }

    std::string formatPitchField (const Line& line)
    {
        if (line.notes.size() == 1)
            return formatPitch (line.notes.front()->pitch, line.notes.front()->spelling);

        std::string field = "[";

        for (const auto* note : line.notes)
        {
            if (field.size() > 1)
                field += ' ';

            field += formatPitch (note->pitch, note->spelling);
        }

        return field + "]";
    }
}

std::string write (const Clip& clip, Meter meter)
{
    assert (clip.lengthInBars >= 1);

    std::string out;
    appendCommentLines (out, clip.header);
    appendCommentLines (out, clip.lengthComments.leading);
    out += "@length " + std::to_string (clip.lengthInBars) + (clip.lengthInBars == 1 ? "bar" : "bars");
    appendTrailingComment (out, clip.lengthComments.trailing);
    out += '\n';

    std::optional<Ticks> previousBar;

    for (const auto& line : buildLines (clip.notes))
    {
        assert (line.length > 0 && line.velocity >= 1 && line.velocity <= 127);

        // A blank line after @length, and between bars.
        if (const auto bar = line.start / meter.ticksPerBar(); bar != previousBar)
        {
            out += '\n';
            previousBar = bar;
        }

        appendCommentLines (out, line.comments.leading);
        appendField (out, formatPosition (line.start, meter), positionWidth);
        appendField (out, formatPitchField (line), pitchWidth);
        appendField (out, formatLength (line.length), lengthWidth);
        out += formatVelocity (line.velocity);
        appendTrailingComment (out, line.comments.trailing);
        out += '\n';
    }

    appendCommentLines (out, clip.footer);
    return out;
}
}
