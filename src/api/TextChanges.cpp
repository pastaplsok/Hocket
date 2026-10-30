#include "api/TextChanges.h"

#include <algorithm>
#include <map>
#include <vector>

namespace hocket::api
{
namespace
{
    constexpr std::string_view byteOrderMark = "\xEF\xBB\xBF";
    constexpr std::size_t maxExamples = 3;

    struct Line
    {
        std::string text;       // as written, without the line ending
        std::string normalized; // trimmed, with each run of spaces and tabs made one space; empty for a blank line
    };

    std::string normalized (std::string_view text)
    {
        std::string out;
        bool pendingSpace = false;

        for (const auto c : text)
        {
            if (c == ' ' || c == '\t')
            {
                pendingSpace = ! out.empty();
                continue;
            }

            if (pendingSpace)
                out += ' ';

            pendingSpace = false;
            out += c;
        }

        return out;
    }

    std::vector<Line> splitLines (std::string_view text)
    {
        std::vector<Line> lines;

        while (! text.empty())
        {
            const auto end = text.find ('\n');
            auto line = text.substr (0, end);

            if (line.ends_with ('\r'))
                line.remove_suffix (1);

            lines.push_back ({ std::string (line), normalized (line) });

            if (end == std::string_view::npos)
                break;

            text.remove_prefix (end + 1);
        }

        return lines;
    }

    std::vector<const Line*> contentLines (const std::vector<Line>& lines)
    {
        std::vector<const Line*> content;

        for (const auto& line : lines)
            if (! line.normalized.empty())
                content.push_back (&line);

        return content;
    }

    // How many blank lines come before each content line, and after the last.
    std::vector<int> blankLineLayout (const std::vector<Line>& lines)
    {
        std::vector<int> layout { 0 };

        for (const auto& line : lines)
        {
            if (line.normalized.empty())
                ++layout.back();
            else
                layout.push_back (0);
        }

        return layout;
    }

    struct Matched
    {
        std::vector<const Line*> common; // in this text's order
        std::vector<const Line*> only;   // lines the other text doesn't have
    };

    // Matches lines by their normalized text, each line of `other` at most once.
    Matched match (const std::vector<const Line*>& lines, const std::vector<const Line*>& other)
    {
        std::map<std::string_view, int> available;

        for (const auto* line : other)
            ++available[line->normalized];

        Matched matched;

        for (const auto* line : lines)
        {
            if (auto& count = available[line->normalized]; count > 0)
            {
                --count;
                matched.common.push_back (line);
            }
            else
            {
                matched.only.push_back (line);
            }
        }

        return matched;
    }

    std::string lineCount (std::size_t count)
    {
        return count == 1 ? "one line" : std::to_string (count) + " lines";
    }

    // "`a`, `b`, `c` and 2 more"
    std::string examples (const std::vector<const Line*>& lines)
    {
        std::string text;

        for (std::size_t i = 0; i < lines.size() && i < maxExamples; ++i)
        {
            if (i > 0)
                text += ", ";

            text += "`" + normalized (lines[i]->text) + "`";
        }

        if (lines.size() > maxExamples)
            text += " and " + std::to_string (lines.size() - maxExamples) + " more";

        return text;
    }

    std::string storedExamples (const std::vector<const Line*>& lines)
    {
        // The stored lines show canonical spacing, which is worth seeing as is.
        std::string text;

        for (std::size_t i = 0; i < lines.size() && i < maxExamples; ++i)
            text += (i > 0 ? ", `" : "`") + lines[i]->text + "`";

        if (lines.size() > maxExamples)
            text += " and " + std::to_string (lines.size() - maxExamples) + " more";

        return text;
    }

    bool sameTexts (const std::vector<const Line*>& a, const std::vector<const Line*>& b)
    {
        return std::ranges::equal (a, b, [] (const Line* x, const Line* y) { return x->normalized == y->normalized; });
    }
}

std::string describeChanges (std::string_view sent, std::string_view stored)
{
    if (sent == stored)
        return {};

    std::vector<std::string> changes;
    const auto hadByteOrderMark = sent.starts_with (byteOrderMark);

    if (hadByteOrderMark)
        sent.remove_prefix (byteOrderMark.size());

    const auto sentLines = splitLines (sent);
    const auto storedLines = splitLines (stored);
    const auto sentContent = contentLines (sentLines);
    const auto storedContent = contentLines (storedLines);

    const auto fromSent = match (sentContent, storedContent);
    const auto fromStored = match (storedContent, sentContent);

    if (! fromSent.only.empty() && ! fromStored.only.empty())
        changes.push_back ("rewrote " + lineCount (fromSent.only.size())
                           + (fromSent.only.size() == fromStored.only.size() ? "" : " as " + (fromStored.only.size() == 1 ? std::string ("one") : std::to_string (fromStored.only.size())))
                           + ": " + examples (fromSent.only) + " became " + storedExamples (fromStored.only));
    else if (! fromStored.only.empty())
        changes.push_back ("added " + storedExamples (fromStored.only));
    else if (! fromSent.only.empty())
        changes.push_back ("dropped " + examples (fromSent.only));

    if (! sameTexts (fromSent.common, fromStored.common))
        changes.push_back ("sorted the lines by position");

    // Lines that match apart from spacing.
    std::vector<std::string> sentSpacing, storedSpacing;

    for (const auto* line : fromSent.common)
        sentSpacing.push_back (line->text);

    for (const auto* line : fromStored.common)
        storedSpacing.push_back (line->text);

    std::ranges::sort (sentSpacing);
    std::ranges::sort (storedSpacing);

    if (sentSpacing != storedSpacing)
        changes.push_back ("changed spacing (columns are fixed-width)");

    // Where blank lines go can only be compared line for line when the content matches.
    const auto blankLinesDiffer = sameTexts (sentContent, storedContent)
                                    ? blankLineLayout (sentLines) != blankLineLayout (storedLines)
                                    : sentLines.size() - sentContent.size() != storedLines.size() - storedContent.size();

    if (blankLinesDiffer)
        changes.push_back ("changed blank lines (one after @length and between bars)");

    if (! sent.empty() && ! sent.ends_with ('\n'))
        changes.push_back ("added the final newline");

    if (sent.find ('\r') != std::string_view::npos)
        changes.push_back ("changed line endings to LF");

    if (hadByteOrderMark)
        changes.push_back ("removed the byte order mark");

    std::string summary;

    for (const auto& change : changes)
        summary += (summary.empty() ? "" : "; ") + change;

    return summary;
}
}
