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

target_link_libraries(hocket_headless PRIVATE hocket_core hocket_engine hocket_api hocket_platform hocket_juce)

hocket_enable_warnings(hocket_headless)
