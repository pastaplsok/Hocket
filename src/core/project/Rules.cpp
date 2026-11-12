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

    std::string number;

    if (std::isfinite (bpm) && std::floor (bpm) == bpm && std::abs (bpm) < 1e15)
    {
        number = std::to_string (static_cast<long long> (bpm));
    }
    else
    {
        char buffer[32] {};
        const auto written = std::to_chars (buffer, buffer + sizeof (buffer), bpm);
        number.assign (buffer, written.ptr);
    }

    return number + " is out of range: the tempo is 20 to 999 BPM (quarter notes per minute), e.g. 120";
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
}
