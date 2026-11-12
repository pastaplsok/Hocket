#include "core/project/Rules.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

namespace hocket::project
{
namespace
{
    std::string singleQuoted (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }

    std::string doubleQuoted (std::string_view text)
    {
        return "\"" + std::string (text) + "\"";
    }

    bool isValidUtf8 (std::string_view text)
    {
        for (std::size_t i = 0; i < text.size();)
        {
            const auto lead = static_cast<unsigned char> (text[i]);

            if (lead < 0x80)
            {
                ++i;
                continue;
            }

            // The sequence's length, and the smallest code point it may encode
            // (anything shorter has an overlong form).
            std::size_t length = 0;
            char32_t codePoint = 0;
            char32_t smallest = 0;

            if ((lead & 0xe0) == 0xc0)
            {
                length = 2;
                codePoint = lead & 0x1f;
                smallest = 0x80;
            }
            else if ((lead & 0xf0) == 0xe0)
            {
                length = 3;
                codePoint = lead & 0x0f;
                smallest = 0x800;
            }
            else if ((lead & 0xf8) == 0xf0)
            {
                length = 4;
                codePoint = lead & 0x07;
                smallest = 0x10000;
            }
            else
            {
                return false;
            }

            if (i + length > text.size())
                return false;

            for (std::size_t j = 1; j < length; ++j)
            {
                const auto next = static_cast<unsigned char> (text[i + j]);

                if ((next & 0xc0) != 0x80)
                    return false;

                codePoint = (codePoint << 6) | (next & 0x3f);
            }

            if (codePoint < smallest || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff))
                return false;

            i += length;
        }

        return true;
    }

    bool isControlCharacter (unsigned char c)
    {
        return c < 0x20 || c == 0x7f;
    }

    std::string_view idExample (IdKind kind)
    {
        switch (kind)
        {
            case IdKind::track: return "trk-bass";
            case IdKind::clip: return "clp-bass-verse";
            case IdKind::device: return "dev-bass-synth";
        }

        return "trk-bass";
    }

    std::string_view idKindName (IdKind kind)
    {
        switch (kind)
        {
            case IdKind::track: return "track";
            case IdKind::clip: return "clip";
            case IdKind::device: return "device";
        }

        return "track";
    }
}

bool isHyphenatedName (std::string_view name)
{
    if (name.empty() || name.front() == '-' || name.back() == '-' || name.find ("--") != std::string_view::npos)
        return false;

    return std::ranges::all_of (name, [] (char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

bool isOneLineText (std::string_view text, std::size_t maxBytes)
{
    return ! text.empty() && text.size() <= maxBytes && isValidUtf8 (text)
        && std::ranges::none_of (text, [] (char c) { return isControlCharacter (static_cast<unsigned char> (c)); });
}

//==============================================================================
std::string_view idPrefix (IdKind kind)
{
    switch (kind)
    {
        case IdKind::track: return "trk-";
        case IdKind::clip: return "clp-";
        case IdKind::device: return "dev-";
    }

    return "trk-";
}

std::optional<std::string> idProblem (std::string_view id, IdKind kind)
{
    const auto kindName = std::string (idKindName (kind));
    const auto prefix = idPrefix (kind);
    const auto example = idExample (kind);

    if (! id.starts_with (prefix))
    {
        for (const auto other : { IdKind::track, IdKind::clip, IdKind::device })
            if (id.starts_with (idPrefix (other)))
                return singleQuoted (id) + " is a " + std::string (idKindName (other)) + " ID; a " + kindName + " ID starts with "
                     + singleQuoted (prefix) + ", e.g. " + singleQuoted (example);

        return singleQuoted (id) + " isn't a valid " + kindName + " ID: " + kindName + " IDs start with " + singleQuoted (prefix)
             + ", e.g. " + singleQuoted (example);
    }

    if (id.size() > maxIdLength)
        return singleQuoted (id) + " is too long: IDs are at most " + std::to_string (maxIdLength) + " characters";

    if (! isHyphenatedName (id.substr (prefix.size())))
        return singleQuoted (id) + " isn't a valid " + kindName + " ID: after " + singleQuoted (prefix)
             + ", use lowercase letters and digits in words joined by single hyphens, e.g. " + singleQuoted (example);

    return {};
}

std::string hyphenatedWords (std::string_view text)
{
    std::string words;
    bool gap = false;

    for (const auto c : text)
    {
        const auto isLetterOrDigit = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');

        if (! isLetterOrDigit)
        {
            gap = true;
            continue;
        }

        if (gap && ! words.empty())
            words += '-';

        words += c >= 'A' && c <= 'Z' ? static_cast<char> (c - 'A' + 'a') : c;
        gap = false;
    }

    return words;
}

std::string unusedId (const Project& project, IdKind kind, std::string_view words, int firstNumber)
{
    const auto prefix = std::string (idPrefix (kind));

    for (auto number = std::max (1, firstNumber);; ++number)
    {
        const auto suffix = number == 1 ? std::string() : "-" + std::to_string (number);
        auto base = std::string (words.substr (0, maxIdLength - prefix.size() - suffix.size()));

        while (! base.empty() && base.back() == '-')
            base.pop_back();

        const auto id = prefix + base + suffix;

        if (! whatHasId (project, id))
            return id;
    }
}

//==============================================================================
std::optional<std::string> trackNameProblem (std::string_view name)
{
    if (isOneLineText (name, maxTrackNameLength))
        return {};

    std::string what;

    if (name.empty())
        what = "this one is empty";
    else if (name.size() > maxTrackNameLength)
        what = "this one is " + std::to_string (name.size()) + " bytes long";
    else if (name.find_first_of ("\r\n") != std::string_view::npos)
        what = "this one has a line break";
    else if (name.find ('\t') != std::string_view::npos)
        what = "this one has a tab";
    else if (! isValidUtf8 (name))
        what = "this one isn't valid UTF-8 text";
    else
        what = "this one has a control character";

    return "a track's name is one line of text, 1 to " + std::to_string (maxTrackNameLength)
         + " bytes with no control characters, e.g. \"Bass\"; " + what;
}

std::optional<std::string> tempoProblem (double bpm)
{
    // Written so that NaN fails too.
    if (bpm >= minTempo && bpm <= maxTempo)
        return {};

    return formatTempo (bpm) + " is out of range: the tempo is 20 to 999 BPM (quarter notes per minute), e.g. 120";
}

std::string formatTempo (double bpm)
{
    if (std::isfinite (bpm) && std::floor (bpm) == bpm && std::abs (bpm) < 1e15)
        return std::to_string (static_cast<long long> (bpm));

    // The shortest text that reads back as the same number.
    char buffer[32] {};
    const auto written = std::to_chars (buffer, buffer + sizeof (buffer), bpm);
    return std::string (buffer, written.ptr);
}

notes::Parsed<TrackType> parseTrackType (std::string_view text)
{
    if (text == "midi")
        return { TrackType::midi, {} };

    if (text == "drums")
        return { TrackType::drums, {} };

    return { {}, doubleQuoted (text) + " tracks aren't supported yet; so far there are \"midi\" and \"drums\" tracks" };
}

//==============================================================================
notes::Parsed<DeviceType> parseDeviceType (std::string_view text)
{
    if (text == "hocket:synth")
        return { DeviceType::synth, {} };

    if (text == "hocket:drum-sampler")
        return { DeviceType::drumSampler, {} };

    if (text == "tracktion:4osc")
        return { {}, doubleQuoted (text) + " is now written \"hocket:synth\": Hocket's synth, which 4OSC plays for now" };

    return { {}, doubleQuoted (text) + " isn't available; so far there are \"hocket:synth\", Hocket's synth, "
                                       "and \"hocket:drum-sampler\", which plays a drum kit" };
}

std::optional<std::string> devicePlacementProblem (DeviceType device, TrackType track)
{
    if (device != DeviceType::drumSampler || track == TrackType::drums)
        return {};

    return "a \"hocket:drum-sampler\" goes on a \"drums\" track, whose drum map names the kit's pieces; this is a \""
         + std::string (trackTypeName (track)) + "\" track";
}

std::optional<std::string> kitNameProblem (std::string_view kit)
{
    if (kit.size() <= maxKitNameLength && isHyphenatedName (kit))
        return {};

    return singleQuoted (kit) + " isn't a valid kit name: kit names are lowercase letters and digits in words joined by single hyphens, "
                                "e.g. \"acoustic\"";
}

//==============================================================================
std::vector<DrumMapProblem> buildDrumMap (std::span<const DrumMapEntry> entries, notes::DrumMap& map)
{
    std::vector<DrumMapProblem> problems;
    std::set<std::string_view> seen; // including names that couldn't go in

    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const auto& [name, note] = entries[i];
        const auto problem = [&] (std::string message, bool aboutNote) { problems.push_back ({ i, std::move (message), aboutNote }); };

        if (! seen.insert (name).second)
            problem (doubleQuoted (name) + " appears twice; a name stands for one note, so keep only one", false);
        else if (auto nameProblem = notes::drumNameProblem (name))
            problem (std::move (*nameProblem), false);
        else if (note < 0 || note > 127)
            problem (doubleQuoted (name) + " should be a MIDI note number from 0 to 127, e.g. 36", true);
        else if (const auto* other = map.nameOf (note))
            problem (doubleQuoted (name) + " and " + doubleQuoted (*other) + " are both note " + std::to_string (note)
                         + ", but a note has one name in a drum map, so that clips are written one way; remove one, or give it another note",
                     true);
        else
            map.add (name, note);
    }

    return problems;
}

//==============================================================================
std::optional<std::string> clipFileProblem (std::string_view file)
{
    // The narrow character set also keeps paths safe and case-insensitive file systems happy.
    constexpr std::string_view folder = "clips/";
    constexpr std::string_view extension = ".notes";

    if (file.starts_with (folder) && file.ends_with (extension) && file.size() > folder.size() + extension.size()
        && isHyphenatedName (file.substr (folder.size(), file.size() - folder.size() - extension.size())))
        return {};

    return singleQuoted (file) + " isn't a valid clip file: clip files are 'clips/<name>.notes' "
                                 "with a lowercase, hyphenated name, e.g. 'clips/bass-verse.notes'";
}

std::optional<std::string> songLengthProblem (notes::Meter meter, notes::Ticks start, notes::Ticks end)
{
    const auto songLimit = maxSongLengthInBars * meter.ticksPerBar();

    if (end <= songLimit)
        return {};

    return "it runs from " + notes::formatPosition (start, meter) + " to " + notes::formatPosition (end, meter) + ", but a song is at most "
         + std::to_string (maxSongLengthInBars) + " bars long, so every clip ends by " + notes::formatPosition (songLimit, meter)
         + "; start it at " + notes::formatPosition (std::max<notes::Ticks> (0, songLimit - (end - start)), meter) + " or earlier";
}

std::optional<std::string> clipPlacementProblem (const Project& project, const Track& track, notes::Ticks start, notes::Ticks length,
                                                 std::string_view except)
{
    const auto meter = project.meter;
    const auto end = start + length;

    if (auto problem = songLengthProblem (meter, start, end))
        return problem;

    struct Span
    {
        const Clip* clip;
        notes::Ticks start;
        notes::Ticks end;
    };

    std::vector<Span> others;

    for (const auto& clip : track.clips)
        if (clip.id != except)
            others.push_back ({ &clip, clip.start, clipEnd (project, clip) });

    std::ranges::sort (others, {}, &Span::start);
    std::vector<std::string> inTheWay;

    for (const auto& other : others)
        if (other.start < end && start < other.end)
            inTheWay.push_back (singleQuoted (other.clip->id) + " (" + notes::formatPosition (other.start, meter) + " to "
                                + notes::formatPosition (other.end, meter) + ")");

    if (inTheWay.empty())
        return {};

    // The free stretches of the track, up to the end of the longest song, and
    // the nearest start in them on either side of the one asked for.
    const auto songLimit = maxSongLengthInBars * meter.ticksPerBar();
    std::optional<notes::Ticks> before;
    std::optional<notes::Ticks> after;
    notes::Ticks freeFrom = 0;

    const auto consider = [&] (notes::Ticks freeStart, notes::Ticks freeEnd)
    {
        if (freeEnd - freeStart < length)
            return;

        const auto latest = freeEnd - length;

        if (freeStart <= start)
            before = std::max (before.value_or (0), std::min (start, latest));

        if (latest >= start)
            after = std::min (after.value_or (songLimit), std::max (start, freeStart));
    };

    for (const auto& other : others)
    {
        consider (freeFrom, other.start);
        freeFrom = std::max (freeFrom, other.end);
    }

    consider (freeFrom, songLimit);

    std::string listed;

    for (std::size_t i = 0; i < inTheWay.size(); ++i)
        listed += (i == 0 ? "" : i + 1 == inTheWay.size() ? " and " : ", ") + inTheWay[i];

    auto message = "it would run from " + notes::formatPosition (start, meter) + " to " + notes::formatPosition (end, meter) + " on " + track.id
                 + ", overlapping " + (inTheWay.size() == 1 ? "clip " : "clips ") + listed + "; clips on a track can't overlap. ";

    if (before && after)
        message += "The nearest starts where it fits are " + notes::formatPosition (*before, meter) + " and " + notes::formatPosition (*after, meter);
    else if (before || after)
        message += "The nearest start where it fits is " + notes::formatPosition (before ? *before : *after, meter);
    else
        message += "There's no room for it anywhere on " + track.id;

    return message;
}

std::string unusedClipFile (const Project& project, std::string_view clipId)
{
    const auto words = std::string (clipId.substr (idPrefix (IdKind::clip).size()));

    for (int number = 1;; ++number)
    {
        auto file = "clips/" + words + (number == 1 ? std::string() : "-" + std::to_string (number)) + ".notes";

        if (! project.notesFiles.contains (file))
            return file;
    }
}
}
