#pragma once

// Reading a JSON file strictly, as project.json and a kit's kit.json are
// read (SPEC §4.4, §8.4): every key required, unknown and duplicate keys
// refused, and each error at its place in the file, written for an LLM
// reader with an example of what's valid. Internal to hocket_core: it
// exposes nlohmann/json, which only core links.

#include "core/project/JsonText.h"
#include "core/project/Project.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hocket::project
{
    struct KeySpec
    {
        std::string_view name;
        std::string_view example; // a valid value, written as JSON
    };

    // A key that belongs on another kind of object, and what to say about it.
    struct MisplacedKey
    {
        std::string_view name;
        std::string_view message;
    };

    // The example given for a key; empty if keys has no such key.
    std::string_view exampleFor (std::span<const KeySpec> keys, std::string_view name);

    // { "id": "trk-bass", ... }, from each key's example.
    std::string exampleObject (std::span<const KeySpec> keys);

    // "'text'" and "\"text\"", for messages.
    std::string singleQuoted (std::string_view text);
    std::string jsonQuoted (std::string_view text);

    // One file's text and the errors found in it, each at its line and column.
    class StrictJson
    {
    public:
        using Json = nlohmann::json;

        // fileName is what errors name: "project.json", "kit.json".
        StrictJson (std::string_view text, std::string fileName);

        // The parsed text, or nullopt after reporting why it isn't valid JSON.
        // Positions are kept, for errors.
        std::optional<Json> parse();

        void errorAtOffset (std::size_t offset, std::string message);
        void errorAt (const Json& value, std::string message);

        // Where a value starts in the text: a string's opening quote.
        std::size_t offsetOf (const Json& value) const;
        int lineOf (const Json& value) const;

        // Reports a value that isn't an object, with an example of one.
        bool requireObject (const Json& value, const std::string& where, std::span<const KeySpec> keys);

        // Duplicate, unknown and missing keys: every key in `keys` is required.
        // A misplaced key gets its own message instead of "unknown key".
        void checkKeys (const Json& object, const std::string& where, std::span<const KeySpec> keys,
                        std::span<const MisplacedKey> misplaced = {});

        static const Json* member (const Json& object, std::string_view key);

        // A member of the type asked for, or nullopt (or nullptr) after
        // reporting what it should be. A missing key is left to checkKeys.
        std::optional<std::string> readString (const Json& object, std::string_view key, const std::string& where,
                                               std::span<const KeySpec> keys);
        const Json* readArray (const Json& object, std::string_view key, const std::string& where, std::span<const KeySpec> keys);
        std::optional<bool> readBool (const Json& object, std::string_view key, const std::string& where);

        // "format" must be `format`, and "formatVersion" exactly `version`:
        // a newer one says it was written by a newer Hocket. `what` names the
        // file's kind for messages: "project", "kit manifest".
        void checkFormat (const Json& root, const std::string& where, std::span<const KeySpec> keys, std::string_view format, int version,
                          std::string_view what);

    protected:
        std::string_view text;
        LineIndex lines;
        std::string fileName;
        std::vector<Diagnostic> errors;
    };
}
