// Planning the song's commands: tempo and time signature.

#include "core/commands/Planning.h"
#include "core/notes/NotesSyntax.h"
#include "core/project/Rules.h"

#include <numeric>

namespace hocket::commands::planning
{
namespace
{
    std::vector<std::string> idsOf (const std::vector<const project::Clip*>& clips)
    {
        std::vector<std::string> ids;

        for (const auto* clip : clips)
            ids.push_back (clip->id);

        return ids;
    }

    std::string joined (const std::vector<std::string>& items, std::string_view separator)
    {
        std::string text;

        for (std::size_t i = 0; i < items.size(); ++i)
            text += (i == 0 ? "" : std::string (separator)) + items[i];

        return text;
    }

    // A file's clips: "clp-a", "clp-b, clp-c (linked)"
    std::string clipsOf (const std::vector<const project::Clip*>& clips)
    {
        return joined (idsOf (clips), ", ") + (clips.size() > 1 ? " (linked)" : "");
    }
}

edits::Plan plan (const project::Project& project, const SetTempo& command)
{
    if (const auto problem = project::tempoProblem (command.bpm))
        return refused ("set the tempo: " + *problem);

    if (command.bpm == project.tempo)
        return unchanged ("the tempo is already " + project::formatTempo (project.tempo) + ", so nothing changed and no history entry was added");

    return { { true, {} }, { edits::SetTempo { command.bpm } }, "set the tempo to " + project::formatTempo (command.bpm), {} };
}

// Every clip and note keeps its time, in ticks; only @length, which is in
// bars, has to change (SPEC §4.4).
edits::Plan plan (const project::Project& project, const SetTimeSignature& command)
{
    const auto parsed = notes::parseMeter (command.timeSignature);

    if (! parsed.value)
        return refused ("set the time signature: " + parsed.error);

    const auto from = project.meter;
    const auto to = *parsed.value;

    if (to == from)
        return unchanged ("the time signature is already " + to.text() + ", so nothing changed and no history entry was added");

    const auto oldBar = from.ticksPerBar();
    const auto newBar = to.ticksPerBar();
    std::vector<std::string> notWhole; // clips that wouldn't be a whole number of the new bars
    std::vector<std::string> tooLong;  // clips that would be over 999 of them
    std::vector<std::string> pastEnd;  // clips that would end after bar 999
    std::vector<edits::Edit> planned { edits::SetMeter { to } };

    for (const auto& [file, content] : project.notesFiles)
    {
        const auto clips = project::clipsUsing (project, file);
        const auto length = content.lengthInBars * oldBar;

        if (length % newBar != 0)
        {
            notWhole.push_back (clipsOf (clips) + ": " + barsText (content.lengthInBars));
            continue;
        }

        const auto bars = static_cast<int> (length / newBar);

        if (bars > notes::maxLengthInBars)
        {
            tooLong.push_back (clipsOf (clips) + " would be " + std::to_string (bars) + " bars long");
            continue;
        }

        if (bars != content.lengthInBars)
        {
            auto resized = content;
            resized.lengthInBars = bars;
            planned.push_back (edits::SetNotes { file, std::move (resized) });
        }
    }

    const auto songLimit = project::maxSongLengthInBars * newBar;

    for (const auto& track : project.tracks)
        for (const auto& clip : track.clips)
            if (const auto end = project::clipEnd (project, clip); end > songLimit)
                pastEnd.push_back (clip.id + " would end at " + notes::formatPosition (end, to));

    if (! notWhole.empty() || ! tooLong.empty() || ! pastEnd.empty())
    {
        std::vector<std::string> problems;

        if (! notWhole.empty())
        {
            // The shortest length that's a whole number of bars in both.
            const auto common = std::lcm (oldBar, newBar);
            const auto oldBars = static_cast<int> (common / oldBar);
            const auto newBars = static_cast<int> (common / newBar);

            problems.push_back ("@length is in bars, and clips keep their length in time, so every clip must be a whole number of " + to.text()
                                + " bars. " + (notWhole.size() == 1 ? "This one isn't: " : "These aren't: ") + joined (notWhole, "; ")
                                + ". In " + from.text() + ", a clip whose @length is a multiple of " + barsText (oldBars) + " fits ("
                                + barsText (oldBars) + " of " + from.text() + " is " + std::to_string (newBars) + " bar"
                                + (newBars == 1 ? "" : "s") + " of " + to.text() + "), so change those clips' @length first, or choose a "
                                  "signature whose bars they fit");
        }

        if (! tooLong.empty())
            problems.push_back ("a clip is at most " + std::to_string (notes::maxLengthInBars) + " bars long, and in " + to.text() + " "
                                + joined (tooLong, "; ") + ". Shorten " + (tooLong.size() == 1 ? "it" : "them") + " first, or choose a longer bar");

        if (! pastEnd.empty())
            problems.push_back ("a song is at most " + std::to_string (project::maxSongLengthInBars) + " bars long, so every clip ends by "
                                + notes::formatPosition (songLimit, to) + ", and in " + to.text() + " " + joined (pastEnd, "; ") + ". Move "
                                + (pastEnd.size() == 1 ? "it" : "them") + " earlier first, or choose a longer bar");

        std::string message = "set the time signature to " + to.text() + ": ";

        for (std::size_t i = 0; i < problems.size(); ++i)
            message += (i == 0 ? "" : ". Also, ") + problems[i];

        return refused (message + ". Nothing was changed");
    }

    // Clips keep their times, so some may now start off a bar line.
    std::vector<const project::Clip*> offBar;

    for (const auto& track : project.tracks)
        for (const auto& clip : track.clips)
            if (clip.start % newBar != 0)
                offBar.push_back (&clip);

    std::string remarks;

    if (offBar.size() == 1)
    {
        remarks = offBar.front()->id + " now starts off a bar line, at " + notes::formatPosition (offBar.front()->start, to);
    }
    else if (! offBar.empty())
    {
        std::vector<std::string> places;

        for (const auto* clip : offBar)
            places.push_back (clip->id + " at " + notes::formatPosition (clip->start, to));

        remarks = "These clips now start off a bar line: " + listText (places);
    }

    return { { true, {} }, std::move (planned), "set the time signature to " + to.text(), std::move (remarks) };
}
}
