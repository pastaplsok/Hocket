#pragma once

// Locations in JSON text, for error messages. nlohmann/json records where each
// value starts and ends, but not where keys are, and it silently keeps only the
// last of two duplicate keys; keysOfObject() fills both gaps.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hocket::project
{
    struct TextPosition
    {
        int line = 0;   // 1-based
        int column = 0; // 1-based, in bytes
    };

    class LineIndex
    {
    public:
        explicit LineIndex (std::string_view text);

        TextPosition positionOf (std::size_t offset) const;

    private:
        std::vector<std::size_t> lineStarts;
    };

    struct KeyAt
    {
        std::string key;        // decoded
        std::size_t offset = 0; // of the key's opening quote
    };

    // The keys of the object whose '{' is at objectStart, in the order written,
    // duplicates included. The text must already have parsed as valid JSON.
    std::vector<KeyAt> keysOfObject (std::string_view text, std::size_t objectStart);

    // The offset of the opening quote of the JSON string whose closing quote is
    // just before end. (nlohmann's start offset for strings is off when they hold escapes.)
    std::size_t stringStart (std::string_view text, std::size_t end);
}
