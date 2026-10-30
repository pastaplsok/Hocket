# End-to-end tests of hocket-headless.

# A project folder whose name is outside every Western ANSI code page, passed on
# the command line: covers argument decoding, path handling and UTF-8 output.
set(unicode_folder "${CMAKE_CURRENT_BINARY_DIR}/ünï ελλ テスト")

add_test(NAME hocket_headless_unicode_project_setup
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/examples/m0-demo" "${unicode_folder}/song")
set_tests_properties(hocket_headless_unicode_project_setup PROPERTIES FIXTURES_SETUP unicode_project)

add_test(NAME hocket_headless_check_unicode_folder
    COMMAND hocket_headless --check "${unicode_folder}/song")
set_tests_properties(hocket_headless_check_unicode_folder PROPERTIES
    FIXTURES_REQUIRED unicode_project
    PASS_REGULAR_EXPRESSION "OK: 120 BPM, 4/4, 1 track, 1 clip")

add_test(NAME hocket_headless_check_reports_unicode_folder
    COMMAND hocket_headless --check "${unicode_folder}/nope")
set_tests_properties(hocket_headless_check_reports_unicode_folder PROPERTIES
    FIXTURES_REQUIRED unicode_project
    PASS_REGULAR_EXPRESSION "ünï ελλ テスト/nope' isn't a folder")
