# hocket-stress: Milestone 0's stress test over the API (StressClient.h). No JUCE.
add_library(hocket_stress_client STATIC
    StressClient.cpp
    StressClient.h)

target_include_directories(hocket_stress_client PUBLIC "${PROJECT_SOURCE_DIR}/tools")
target_link_libraries(hocket_stress_client PUBLIC hocket_api hocket_core nlohmann_json::nlohmann_json)
hocket_enable_warnings(hocket_stress_client)

add_executable(hocket_stress Main.cpp)
set_target_properties(hocket_stress PROPERTIES OUTPUT_NAME "hocket-stress")
target_link_libraries(hocket_stress PRIVATE hocket_stress_client hocket_api hocket_platform)
hocket_enable_warnings(hocket_stress)
