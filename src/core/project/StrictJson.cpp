#include "core/project/StrictJson.h"

#include "core/Suggestions.h"

#include <algorithm>
#include <cstdint>
#include <set>

namespace hocket::project
{
std::string_view exampleFor (std::span<const KeySpec> keys, std::string_view name)
{
    for (const auto& key : keys)
        if (key.name == name)
            return key.example;

    return {};
}

std::string exampleObject (std::span<const KeySpec> keys)
{
    std::string text = "{ ";

    for (const auto& key : keys)
        text += (text.size() > 2 ? ", " : "") + jsonQuoted (key.name) + ": " + std::string (key.example);

    return text + " }";
}

std::string singleQuoted (std::string_view text)
{
    return "'" + std::string (text) + "'";
}

std::string jsonQuoted (std::string_view text)
{
    return "\"" + std::string (text) + "\"";
}

//==============================================================================
StrictJson::StrictJson (std::string_view jsonText, std::string name)
    : text (jsonText), lines (jsonText), fileName (std::move (name))
{
}

std::optional<StrictJson::Json> StrictJson::parse()
{
    // Constructed in place: the value positions must come through intact.
    std::optional<Json> root;

    try
    {
        root.emplace (Json::parse (text));
    }
    catch (const Json::parse_error& e)
    {
        // e.what() is "[json.exception.parse_error.101] parse error at line 3, column 5: <detail>"
        std::string detail = e.what();

        if (const auto column = detail.find ("column "); column != std::string::npos)
            if (const auto colon = detail.find (": ", column); colon != std::string::npos)
                detail = detail.substr (colon + 2);

        const auto offset = std::min (e.byte > 0 ? e.byte - 1 : 0, text.size());
        const auto at = offset < text.size() ? text[offset] : '\0';
        auto before = offset;

        while (before > 0 && std::string_view (" \t\r\n").find (text[before - 1]) != std::string_view::npos)
            --before;

        if ((at == '}' || at == ']') && before > 0 && text[before - 1] == ',')
            detail += " (JSON doesn't allow a comma before a closing bracket)";
        else if (at == '/' || (offset > 0 && text[offset - 1] == '/'))
            detail += " (JSON doesn't allow comments)";

        errorAtOffset (offset, fileName + " isn't valid JSON: " + detail);
        return {};
    }

    return root;
}

void StrictJson::errorAtOffset (std::size_t offset, std::string message)
{
    const auto position = lines.positionOf (offset);
    errors.push_back ({ fileName, position.line, position.column, std::move (message) });
}

void StrictJson::errorAt (const Json& value, std::string message)
{
    errorAtOffset (offsetOf (value), std::move (message));
}

std::size_t StrictJson::offsetOf (const Json& value) const
{
    return value.is_string() ? stringStart (text, value.end_pos()) : value.start_pos();
}

int StrictJson::lineOf (const Json& value) const
{
    return lines.positionOf (offsetOf (value)).line;
}

bool StrictJson::requireObject (const Json& value, const std::string& where, std::span<const KeySpec> keys)
{
    if (value.is_object())
        return true;

    errorAt (value, where + " should be an object: " + exampleObject (keys));
    return false;
}

void StrictJson::checkKeys (const Json& object, const std::string& where, std::span<const KeySpec> keys, std::span<const MisplacedKey> misplaced)
{
    std::set<std::string> seen;

    for (const auto& [key, offset] : keysOfObject (text, object.start_pos()))
    {
        if (! seen.insert (key).second)
        {
            errorAtOffset (offset, where + ": " + jsonQuoted (key) + " appears twice; keep only one");
            continue;
        }

        if (! exampleFor (keys, key).empty())
            continue;

        if (const auto found = std::ranges::find (misplaced, key, &MisplacedKey::name); found != misplaced.end())
        {
            errorAtOffset (offset, where + ": " + std::string (found->message));
            continue;
        }

        std::string message = where + ": unknown key " + jsonQuoted (key);

        for (const auto& spec : keys)
            if (editDistance (key, spec.name) <= 2)
                message += " (did you mean " + jsonQuoted (spec.name) + "?)";

        message += "; the keys are ";

        for (std::size_t i = 0; i < keys.size(); ++i)
            message += (i > 0 ? ", " : "") + jsonQuoted (keys[i].name);

        errorAtOffset (offset, std::move (message));
    }

    for (const auto& spec : keys)
        if (! object.contains (spec.name))
            errorAt (object, where + ": missing " + jsonQuoted (spec.name) + "; add e.g. " + jsonQuoted (spec.name) + ": " + std::string (spec.example));
}

const StrictJson::Json* StrictJson::member (const Json& object, std::string_view key)
{
    const auto found = object.find (key);
    return found == object.end() ? nullptr : &*found;
}

std::optional<std::string> StrictJson::readString (const Json& object, std::string_view key, const std::string& where,
                                                   std::span<const KeySpec> keys)
{
    const auto* value = member (object, key);

    if (value == nullptr)
        return {}; // reported by checkKeys

    if (! value->is_string())
    {
        errorAt (*value, where + ": " + jsonQuoted (key) + " should be a string, e.g. " + std::string (exampleFor (keys, key)));
        return {};
    }

    return value->get<std::string>();
}

const StrictJson::Json* StrictJson::readArray (const Json& object, std::string_view key, const std::string& where, std::span<const KeySpec> keys)
{
    const auto* value = member (object, key);

    if (value == nullptr)
        return nullptr;

    if (! value->is_array())
    {
        errorAt (*value, where + ": " + jsonQuoted (key) + " should be an array, e.g. " + std::string (exampleFor (keys, key)));
        return nullptr;
    }

    return value;
}

std::optional<bool> StrictJson::readBool (const Json& object, std::string_view key, const std::string& where)
{
    const auto* value = member (object, key);

    if (value == nullptr)
        return {}; // reported by checkKeys

    if (! value->is_boolean())
    {
        errorAt (*value, where + ": " + jsonQuoted (key) + " should be true or false, without quotes");
        return {};
    }

    return value->get<bool>();
}

void StrictJson::checkFormat (const Json& root, const std::string& where, std::span<const KeySpec> keys, std::string_view format, int version,
                              std::string_view what)
{
    if (const auto written = readString (root, "format", where, keys); written && *written != format)
        errorAt (root.at ("format"), where + ": \"format\" must be " + jsonQuoted (format) + "; this doesn't look like a Hocket " + std::string (what)
                                         + " file");

    if (const auto* written = member (root, "formatVersion"))
    {
        if (! written->is_number_integer())
            errorAt (*written, where + ": \"formatVersion\" should be a whole number, e.g. " + std::to_string (version));
        else if (const auto number = written->get<std::int64_t>(); number > version)
            errorAt (*written, where + ": the " + std::string (what) + " is format version " + std::to_string (number)
                                   + ", but this build of Hocket reads only format version " + std::to_string (version)
                                   + "; it was written by a newer Hocket");
        else if (number < version)
            errorAt (*written, where + ": \"formatVersion\" must be " + std::to_string (version));
    }
}
}
