cmake_minimum_required(VERSION 3.25)

project(Hocket
    VERSION 0.0.1
    DESCRIPTION "An AI-native DAW where a human and an LLM co-produce music"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(MSVC)
    # Source files and project text are UTF-8 (e.g. the → in .notes/.auto shapes).
    add_compile_options(/utf-8)
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(HocketWarnings)
include(HocketDependencies)

option(HOCKET_BUILD_TESTS "Build Hocket's tests" ON)

add_subdirectory(src)

if(HOCKET_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
