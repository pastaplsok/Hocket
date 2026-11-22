add_executable(hocket_storage_tests
    FileTests.cpp
    InterruptedSaveTests.cpp
    KeptSaveTests.cpp
    OutsideChangeTests.cpp
    ProjectFolderTests.cpp
    ProjectWatcherTests.cpp
    RecoveryTests.cpp
    SaveKeysTests.cpp
    StorageTesting.h)

target_link_libraries(hocket_storage_tests PRIVATE hocket_storage hocket_platform doctest::doctest_with_main)
target_include_directories(hocket_storage_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")

target_compile_definitions(hocket_storage_tests PRIVATE
    HOCKET_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")

hocket_enable_warnings(hocket_storage_tests)
hocket_long_paths(hocket_storage_tests)

add_test(NAME hocket_storage_tests COMMAND hocket_storage_tests)
