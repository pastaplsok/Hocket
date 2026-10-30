# hocket_platform: operating-system services behind one interface (Platform.h),
# so ports to other systems add a file instead of editing callers (SPEC §9).
add_library(hocket_platform STATIC
    Platform.h)

if(WIN32)
    target_sources(hocket_platform PRIVATE Platform_windows.cpp)
    target_link_libraries(hocket_platform PRIVATE bcrypt ws2_32 shell32 ole32)
else()
    target_sources(hocket_platform PRIVATE Platform_posix.cpp)
endif()

target_link_libraries(hocket_platform PUBLIC hocket_core)

hocket_enable_warnings(hocket_platform)
