# Warning flags for Hocket's own targets. Never apply these to third-party code:
# JUCE and Tracktion are compiled in hocket_juce, which doesn't use this function.
#
# Headers included with <angle brackets> (JUCE, Tracktion, other external/ libs)
# are treated as external, so their warnings don't drown out ours.
function(hocket_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /external:anglebrackets
            /external:W0)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
