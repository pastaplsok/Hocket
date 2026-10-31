# What tools/acceptance/Run-Acceptance.ps1 makes of a --play run, checked
# without running one. Windows PowerShell only, like the script itself.
if(WIN32)
    add_test(NAME acceptance_checks
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/AcceptanceChecksTests.ps1")
endif()
