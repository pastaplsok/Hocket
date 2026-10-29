# Third-party dependencies. Each is a git submodule under external/, pinned to an
# exact commit; DEPENDENCIES.md lists versions and licenses.

set(HOCKET_EXTERNAL_DIR "${PROJECT_SOURCE_DIR}/external")

foreach(dep IN ITEMS JUCE tracktion_engine IXWebSocket json doctest)
    if(NOT EXISTS "${HOCKET_EXTERNAL_DIR}/${dep}/CMakeLists.txt")
        message(FATAL_ERROR
            "external/${dep} is missing. From the repo root, run:\n"
            "  git submodule update --init\n"
            "(not --recursive: JUCE is our own submodule, see DEPENDENCIES.md)")
    endif()
endforeach()

# Let our plain variables override the subprojects' option() defaults.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# ---- JUCE + Tracktion Engine ----------------------------------------------------

add_subdirectory("${HOCKET_EXTERNAL_DIR}/JUCE" EXCLUDE_FROM_ALL)
add_subdirectory("${HOCKET_EXTERNAL_DIR}/tracktion_engine/modules" EXCLUDE_FROM_ALL)

# Our JUCE pin must match the one Tracktion was built and tested against.
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${HOCKET_EXTERNAL_DIR}/tracktion_engine" rev-parse HEAD:modules/juce
        OUTPUT_VARIABLE te_juce_sha
        RESULT_VARIABLE te_juce_result
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${HOCKET_EXTERNAL_DIR}/JUCE" rev-parse HEAD
        OUTPUT_VARIABLE juce_sha
        RESULT_VARIABLE juce_result
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

    if(te_juce_result EQUAL 0 AND juce_result EQUAL 0)
        if(NOT te_juce_sha STREQUAL juce_sha)
            message(WARNING
                "JUCE pin mismatch: external/JUCE is at ${juce_sha}, but Tracktion Engine "
                "expects ${te_juce_sha}. Check out Tracktion's pin in external/JUCE "
                "(see DEPENDENCIES.md).")
        endif()
    endif()
endif()

# Engine::getVersion() is a hardcoded string that upstream doesn't keep current,
# so take the version from Tracktion's VERSION.md instead.
file(READ "${HOCKET_EXTERNAL_DIR}/tracktion_engine/VERSION.md" HOCKET_TRACKTION_VERSION)
string(STRIP "${HOCKET_TRACKTION_VERSION}" HOCKET_TRACKTION_VERSION)

# hocket_juce compiles JUCE and Tracktion once, as a static library, so every Hocket
# target (app, engine tests) links the same build instead of recompiling the modules.
# Module config flags are PUBLIC so every translation unit sees the same settings.
add_library(hocket_juce STATIC)

target_link_libraries(hocket_juce
    PRIVATE
        tracktion::tracktion_core
        tracktion::tracktion_graph
        tracktion::tracktion_engine
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags)

target_compile_definitions(hocket_juce PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    # Plugin hosting is Phase 2 (SPEC §10.3).
    JUCE_PLUGINHOST_VST3=0
    JUCE_PLUGINHOST_AU=0
    # Windows audio: ASIO + WASAPI only (SPEC §9). JUCE bundles the ASIO SDK headers (GPLv3 option).
    $<$<PLATFORM_ID:Windows>:JUCE_ASIO=1>
    $<$<PLATFORM_ID:Windows>:JUCE_DIRECTSOUND=0>
    # Hocket prints its own device list.
    TRACKTION_LOG_DEVICES=0)

# Hand the modules' include paths and definitions (including those inherited from
# the JUCE module targets) on to anything that links hocket_juce.
target_include_directories(hocket_juce INTERFACE
    $<TARGET_PROPERTY:hocket_juce,INCLUDE_DIRECTORIES>)
target_compile_definitions(hocket_juce INTERFACE
    $<TARGET_PROPERTY:hocket_juce,COMPILE_DEFINITIONS>)

set_target_properties(hocket_juce PROPERTIES FOLDER "external")

# ---- Small libraries (used from Milestone 0 step 2 on) --------------------------

set(USE_TLS OFF)               # localhost-only API (SPEC §6.3)
set(USE_ZLIB OFF)
set(IXWEBSOCKET_INSTALL OFF)
add_subdirectory("${HOCKET_EXTERNAL_DIR}/IXWebSocket" EXCLUDE_FROM_ALL)

set(JSON_Install OFF)
add_subdirectory("${HOCKET_EXTERNAL_DIR}/json" EXCLUDE_FROM_ALL)

set(DOCTEST_NO_INSTALL ON)
# doctest's forward-declared std::ostream breaks printing std::string_view on MSVC.
set(DOCTEST_USE_STD_HEADERS ON)
add_subdirectory("${HOCKET_EXTERNAL_DIR}/doctest" EXCLUDE_FROM_ALL)
