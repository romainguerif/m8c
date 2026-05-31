# Fetch JUCE for the embedded plugin host. Pinned to Element's version so the
# PDC / DelayChannelOp lift stays source-compatible.
include(FetchContent)

set(M8C_JUCE_VERSION 8.0.12)

if(NOT TARGET juce::juce_core)
    find_package(JUCE ${M8C_JUCE_VERSION} CONFIG QUIET)
    if(NOT JUCE_FOUND)
        message(STATUS "Fetching JUCE ${M8C_JUCE_VERSION} (first configure is slow)")
        FetchContent_Declare(juce
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG ${M8C_JUCE_VERSION}
            GIT_SHALLOW ON
            EXCLUDE_FROM_ALL)
        FetchContent_MakeAvailable(juce)
    endif()
endif()
