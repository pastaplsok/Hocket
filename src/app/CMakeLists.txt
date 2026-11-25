juce_add_console_app(hocket_headless
    PRODUCT_NAME "hocket-headless"
    COMPANY_NAME "Hocket")

# PRODUCT_NAME doesn't rename console apps' output, so set it directly.
set_target_properties(hocket_headless PROPERTIES OUTPUT_NAME "hocket-headless")

target_sources(hocket_headless PRIVATE
    Main.cpp
    Console.h)

if(WIN32)
    target_sources(hocket_headless PRIVATE Console_windows.cpp)
else()
    target_sources(hocket_headless PRIVATE Console_posix.cpp)
endif()

target_compile_definitions(hocket_headless PRIVATE
    HOCKET_VERSION="${PROJECT_VERSION}"
    HOCKET_TRACKTION_VERSION="${HOCKET_TRACKTION_VERSION}")

target_link_libraries(hocket_headless PRIVATE hocket_core hocket_engine hocket_api hocket_platform hocket_storage hocket_juce)

hocket_enable_warnings(hocket_headless)
hocket_long_paths(hocket_headless)

# The bundled drum kits (SPEC §8.4) go in kits\ next to the program, where it
# looks for them. Copied afresh on every build (about 1 MB), so a piece renamed
# or removed in assets/kits doesn't linger.
add_custom_target(hocket_kits ALL
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "$<TARGET_FILE_DIR:hocket_headless>/kits"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/assets/kits" "$<TARGET_FILE_DIR:hocket_headless>/kits"
    COMMENT "Copying the bundled drum kits next to hocket-headless"
    VERBATIM)
add_dependencies(hocket_kits hocket_headless)
