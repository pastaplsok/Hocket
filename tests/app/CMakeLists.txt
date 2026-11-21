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

# A drum project names a kit, but reads and validates without any kit present
# (SPEC §5.2): its drum map is in project.json.
add_test(NAME hocket_headless_drum_project_setup
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/tests/fixtures/drum-project" "${unicode_folder}/drums")
set_tests_properties(hocket_headless_drum_project_setup PROPERTIES FIXTURES_SETUP drum_project)

add_test(NAME hocket_headless_check_drum_project
    COMMAND hocket_headless --check "${unicode_folder}/drums")
set_tests_properties(hocket_headless_check_drum_project PROPERTIES
    FIXTURES_REQUIRED drum_project
    PASS_REGULAR_EXPRESSION "OK: 174 BPM, 4/4, 2 tracks, 4 clips in 3 files, as linked clips share them")

# Its kit isn't installed, which --check notes without failing (SPEC §8.4).
add_test(NAME hocket_headless_check_notes_missing_kit
    COMMAND hocket_headless --check "${unicode_folder}/drums")
set_tests_properties(hocket_headless_check_notes_missing_kit PROPERTIES
    FIXTURES_REQUIRED drum_project
    PASS_REGULAR_EXPRESSION "Note: track trk-drums is silent: kit 'not-installed' isn't installed")

# The electronic kit's audition project (step 8) is valid, and its kit is
# installed: the build copies the bundled kits next to the program.
add_test(NAME hocket_headless_check_electronic_kit_example
    COMMAND hocket_headless --check "${PROJECT_SOURCE_DIR}/examples/electronic-kit")
set_tests_properties(hocket_headless_check_electronic_kit_example PROPERTIES
    PASS_REGULAR_EXPRESSION "OK: 96 BPM, 4/4, 1 track, 4 clips in 3 files, as linked clips share them"
    FAIL_REGULAR_EXPRESSION "silent|isn't installed")

# A --master-volume that isn't a volume is refused before anything else. The
# folder doesn't exist either, so --play can't get as far as opening a device.
add_test(NAME hocket_headless_master_volume_needs_a_unit
    COMMAND hocket_headless --play "${unicode_folder}/nope" --master-volume -12)
set_tests_properties(hocket_headless_master_volume_needs_a_unit PROPERTIES
    FIXTURES_REQUIRED unicode_project
    TIMEOUT 30
    PASS_REGULAR_EXPRESSION "--master-volume: '-12' isn't a valid volume: it has no unit; volume is written in dB")

# --recover and --discard-recovery (SPEC §4.5) are refused together, before
# anything else: before looking for a recovery copy, or opening a device.
add_test(NAME hocket_headless_recover_or_discard
    COMMAND hocket_headless --play "${unicode_folder}/nope" --recover --discard-recovery)
set_tests_properties(hocket_headless_recover_or_discard PROPERTIES
    FIXTURES_REQUIRED unicode_project
    TIMEOUT 30
    PASS_REGULAR_EXPRESSION "--recover and --discard-recovery don't go together")

# --new makes an empty project in a new folder (SPEC §4.5), which --check
# finds valid, and refuses a folder with anything in it. The folder is
# removed first, so each run makes it afresh.
set(new_project_folder "${unicode_folder}/new ünï/song")

add_test(NAME hocket_headless_new_project_setup
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${unicode_folder}/new ünï")
set_tests_properties(hocket_headless_new_project_setup PROPERTIES FIXTURES_SETUP new_project_clean)

add_test(NAME hocket_headless_new_project
    COMMAND hocket_headless --new "${new_project_folder}" --user Lee)
set_tests_properties(hocket_headless_new_project PROPERTIES
    FIXTURES_REQUIRED new_project_clean
    FIXTURES_SETUP new_project
    PASS_REGULAR_EXPRESSION "Made an empty project in .*new ünï.song: 120 BPM, 4/4, no tracks")

add_test(NAME hocket_headless_check_new_project
    COMMAND hocket_headless --check "${new_project_folder}")
set_tests_properties(hocket_headless_check_new_project PROPERTIES
    FIXTURES_REQUIRED new_project
    PASS_REGULAR_EXPRESSION "OK: 120 BPM, 4/4, 0 tracks, 0 clips")

add_test(NAME hocket_headless_new_refuses_a_project
    COMMAND hocket_headless --new "${new_project_folder}")
set_tests_properties(hocket_headless_new_refuses_a_project PROPERTIES
    FIXTURES_REQUIRED new_project
    WILL_FAIL TRUE)

add_test(NAME hocket_headless_new_says_why_it_refuses
    COMMAND hocket_headless --new "${new_project_folder}")
set_tests_properties(hocket_headless_new_says_why_it_refuses PROPERTIES
    FIXTURES_REQUIRED new_project
    PASS_REGULAR_EXPRESSION "song' isn't empty: it holds a project already")
