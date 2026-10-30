#include "core/project/JsonText.h"

#include <doctest/doctest.h>

using namespace hocket::project;

TEST_CASE ("line index turns byte offsets into 1-based line and column")
{
    const LineIndex lines ("ab\ncd\n\nef");

    CHECK (lines.positionOf (0).line == 1);
    CHECK (lines.positionOf (0).column == 1);
    CHECK (lines.positionOf (1).column == 2);
    CHECK (lines.positionOf (3).line == 2);
    CHECK (lines.positionOf (3).column == 1);
    CHECK (lines.positionOf (6).line == 3);
    CHECK (lines.positionOf (7).line == 4);
    CHECK (lines.positionOf (8).column == 2);
}

TEST_CASE ("keysOfObject finds top-level keys, duplicates included, and skips nested ones")
{
    const std::string_view text = R"({"a": {"b": 1}, "c": [1, {"d": "}"}], "a": "x,y", "tempo": 2})";
    const auto keys = keysOfObject (text, 0);

    REQUIRE (keys.size() == 4);
    CHECK (keys[0].key == "a");
    CHECK (keys[0].offset == 1);
    CHECK (keys[1].key == "c");
    CHECK (keys[2].key == "a");
    CHECK (text.substr (keys[2].offset, 3) == "\"a\"");
    CHECK (keys[3].key == "tempo"); // escapes are decoded

    const auto nested = keysOfObject (text, text.find ("{\"b\""));
    REQUIRE (nested.size() == 1);
    CHECK (nested[0].key == "b");
}

TEST_CASE ("stringStart finds the opening quote even past escaped quotes")
{
    const std::string_view text = R"({"k": "a\"b\\"})";
    const auto end = text.size() - 1; // just after the closing quote of the value
    CHECK (stringStart (text, end) == text.find ("\"a"));
}
