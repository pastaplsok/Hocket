#include "api/TextChanges.h"
#include "core/notes/Notes.h"

#include <doctest/doctest.h>

using namespace hocket;
using api::describeChanges;

namespace
{
    // What the bus stores for this text, written back out.
    std::string canonical (std::string_view text)
    {
        const auto parsed = notes::parse (text);
        REQUIRE (parsed.clip);
        return notes::write (*parsed.clip);
    }

    std::string changesFor (std::string_view sent)
    {
        return describeChanges (sent, canonical (sent));
    }
}

TEST_CASE ("canonical text has no changes")
{
    const std::string text = "@length 1bar\n\n1.1      C2    1/4   v100\n";
    REQUIRE (canonical (text) == text);
    CHECK (changesFor (text).empty());
}

TEST_CASE ("spacing inside lines")
{
    CHECK (changesFor ("@length 1bar\n\n1.1 C2 1/4 v100\n") == "changed spacing (columns are fixed-width)");
    CHECK (changesFor ("@length 1bar\n\n1.1      C2    1/4   v100   \n") == "changed spacing (columns are fixed-width)");
}

TEST_CASE ("blank lines and the final newline")
{
    CHECK (changesFor ("@length 2bars\n\n1.1      C2    1/4   v100\n2.1      D2    1/4   v100")
           == "changed blank lines (one after @length and between bars); added the final newline");
}

TEST_CASE ("lines in the wrong order")
{
    CHECK (changesFor ("@length 1bar\n\n1.2      D2    1/4   v100\n1.1      C2    1/4   v100\n") == "sorted the lines by position");
}

TEST_CASE ("rewritten lines say what they became")
{
    CHECK (changesFor ("@length 1bar\n\n1.1.1    C2    2/8   v100\n") == "rewrote one line: `1.1.1 C2 2/8 v100` became `1.1      C2    1/4   v100`");

    CHECK (changesFor ("@length 1bar\n\n1.1      C3    1/2   v80\n1.1      E3    1/2   v80\n")
           == "rewrote 2 lines as one: `1.1 C3 1/2 v80`, `1.1 E3 1/2 v80` became `1.1      [C3 E3]  1/2   v80`");
}

TEST_CASE ("a missing @length line is added")
{
    CHECK (changesFor ("1.1      C2    1/4   v100\n") == "added `@length 1bar`; changed blank lines (one after @length and between bars)");
}

TEST_CASE ("line endings and the byte order mark")
{
    CHECK (changesFor ("\xEF\xBB\xBF@length 1bar\r\n\r\n1.1      C2    1/4   v100\r\n") == "changed line endings to LF; removed the byte order mark");
}

TEST_CASE ("a summary lists at most three examples")
{
    const auto changes = changesFor ("@length 1bar\n\n1.1.1 C2 2/8 v100\n1.2.1 C2 2/8 v100\n1.3.1 C2 2/8 v100\n1.4.1 C2 2/8 v100\n1.4.3 C2 2/16 v100\n");

    CHECK (changes.starts_with ("rewrote 5 lines: `1.1.1 C2 2/8 v100`, `1.2.1 C2 2/8 v100`, `1.3.1 C2 2/8 v100` and 2 more became "
                                "`1.1      C2    1/4   v100`, `1.2      C2    1/4   v100`, `1.3      C2    1/4   v100` and 2 more"));
}
