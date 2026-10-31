# Runs hocket-mcp as an MCP client does and checks its stdio: one JSON reply
# per request, each on a line of its own ending in LF (no CR), nothing else on
# stdout, and a clean exit when stdin closes. There's no token file, so tool
# calls say Hocket isn't running.
#
#   cmake -DHOCKET_MCP=<exe> -DWORK_DIR=<folder> -DLABEL=<label> -P StdioTest.cmake

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# A byte order mark, a blank line and a CRLF line, which the bridge must take too.
set(input "${WORK_DIR}/input.jsonl")
string(ASCII 239 187 191 bom)
file(WRITE "${input}"
    "${bom}{\"jsonrpc\": \"2.0\", \"id\": 1, \"method\": \"initialize\", \"params\": {\"protocolVersion\": \"2025-11-25\", \"capabilities\": {}, \"clientInfo\": {\"name\": \"claude-code\", \"version\": \"1.0\"}}}\n"
    "${bom}\n"
    "{\"jsonrpc\": \"2.0\", \"method\": \"notifications/initialized\"}\n"
    "\n"
    "{\"jsonrpc\": \"2.0\", \"id\": 2, \"method\": \"tools/list\"}\r\n"
    "{\"jsonrpc\": \"2.0\", \"id\": \"three\", \"method\": \"tools/call\", \"params\": {\"name\": \"get_project_overview\", \"arguments\": {}}}\n")

execute_process(
    COMMAND "${HOCKET_MCP}" --token-file "${WORK_DIR}/api.json" --label "${LABEL}"
    INPUT_FILE "${input}"
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE result
    TIMEOUT 30
    ENCODING UTF-8)

function(fail message)
    message(FATAL_ERROR "${message}\n--- stdout:\n${out}\n--- stderr:\n${err}")
endfunction()

if(NOT result EQUAL 0)
    fail("hocket-mcp exited with ${result}")
endif()

string(FIND "${out}" "\r" cr)

if(NOT cr EQUAL -1)
    fail("stdout has a CR: Windows text mode is translating line endings")
endif()

# Lines can hold semicolons, so they're taken one at a time rather than as a list.
set(rest "${out}")
set(ids "")

while(NOT rest STREQUAL "")
    string(FIND "${rest}" "\n" end)

    if(end EQUAL -1)
        fail("the last line on stdout doesn't end in a newline")
    endif()

    string(SUBSTRING "${rest}" 0 ${end} line)
    math(EXPR next "${end} + 1")
    string(SUBSTRING "${rest}" ${next} -1 rest)

    string(JSON id ERROR_VARIABLE error GET "${line}" id)

    if(error)
        fail("a line on stdout isn't a JSON-RPC reply: ${line}")
    endif()

    list(APPEND ids "${id}")

    if(id STREQUAL "2")
        string(JSON tool_count LENGTH "${line}" result tools)

        if(NOT tool_count EQUAL 9)
            fail("tools/list has ${tool_count} tools, not 9")
        endif()
    elseif(id STREQUAL "three")
        string(JSON is_error GET "${line}" result isError)
        string(JSON text GET "${line}" result content 0 text)

        if(NOT is_error OR NOT text MATCHES "^Hocket isn't running")
            fail("the tool call should say Hocket isn't running")
        endif()
    endif()
endwhile()

if(NOT ids STREQUAL "1;2;three")
    fail("expected replies to requests 1, 2 and three, in order, not: ${ids}")
endif()

foreach(expected IN ITEMS "label '${LABEL}'" "${LABEL}/api.json" "client 'claude-code' 1.0" "stdin closed")
    string(FIND "${err}" "${expected}" found)

    if(found EQUAL -1)
        fail("stderr doesn't mention: ${expected}")
    endif()
endforeach()
