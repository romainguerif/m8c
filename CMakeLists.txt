cmake_minimum_required(VERSION 3.20)

project(m8c LANGUAGES C CXX VERSION 2.2.4)

# The embedded JUCE plugin host adds C++ (and Obj-C++ on macOS).
if (APPLE)
    enable_language(OBJCXX)
endif ()

set(APP_NAME m8c)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(USE_LIBSERIALPORT "Use libserialport as a backend" OFF)
option(USE_LIBUSB "Use libusb as a backend" OFF)
option(USE_RTMIDI "Use RtMidi as a backend" OFF)

# Enable USE_LIBSERIALPORT by default if neither USE_LIBUSB nor USE_RTMIDI are defined
if (NOT USE_LIBUSB AND NOT USE_RTMIDI)
    message(STATUS "Neither USE_LIBUSB nor USE_RTMIDI are enabled. Enabling USE_LIBSERIALPORT by default.")
    set(USE_LIBSERIALPORT ON)
endif ()


file(GLOB m8c_SRC "src/*.h" "src/*.c" "src/backends/*.h" "src/backends/*.c" "src/fonts/*.h" "src/fonts/*.c")

set(MACOS_CONTENTS "${CMAKE_CURRENT_SOURCE_DIR}/package/macos/m8c.app/Contents")
set(MACOS_ENTITLEMENTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/package/macos/Entitlements.plist")

set(APP_ICON ${MACOS_CONTENTS}/Resources/m8c.icns)
set_source_files_properties(${APP_ICON} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")

if (APPLE)
    add_executable(${APP_NAME} MACOSX_BUNDLE ${APP_ICON} ${m8c_SRC})
elseif (WIN32)
    add_executable(${APP_NAME} WIN32 ${m8c_SRC})
else ()
    add_executable(${APP_NAME} ${m8c_SRC})
endif ()

find_package(PkgConfig REQUIRED)
find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3)

pkg_check_modules(SDL3 REQUIRED sdl3)

target_link_options(${APP_NAME} PRIVATE ${SDL3_LDFLAGS})
target_include_directories(${APP_NAME} PRIVATE ${SDL3_INCLUDE_DIRS})
target_compile_options(${APP_NAME} PRIVATE ${SDL3_CFLAGS_OTHER})

if (USE_LIBSERIALPORT)
    pkg_check_modules(LIBSERIALPORT REQUIRED libserialport)
    target_link_options(${APP_NAME} PRIVATE ${LIBSERIALPORT_LDFLAGS})
    target_include_directories(${APP_NAME} PRIVATE ${LIBSERIALPORT_INCLUDE_DIRS})
    target_compile_options(${APP_NAME} PRIVATE ${LIBSERIALPORT_CFLAGS_OTHER})
    target_compile_definitions(${APP_NAME} PRIVATE USE_LIBSERIALPORT)
endif ()

if (USE_LIBUSB)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
    target_link_options(${APP_NAME} PRIVATE ${LIBUSB_LDFLAGS})
    target_include_directories(${APP_NAME} PRIVATE ${LIBUSB_INCLUDE_DIRS})
    target_compile_options(${APP_NAME} PRIVATE ${LIBUSB_CFLAGS_OTHER})
    target_compile_definitions(${APP_NAME} PRIVATE USE_LIBUSB)
endif ()

if (USE_RTMIDI)
    pkg_check_modules(RTMIDI REQUIRED rtmidi)
    target_link_options(${APP_NAME} PRIVATE ${RTMIDI_LDFLAGS})
    target_include_directories(${APP_NAME} PRIVATE ${RTMIDI_INCLUDE_DIRS})
    target_compile_options(${APP_NAME} PRIVATE ${RTMIDI_CFLAGS_OTHER})
    target_compile_definitions(${APP_NAME} PRIVATE USE_RTMIDI)
endif ()

if (WIN32)
    target_link_libraries(${APP_NAME} ${SDL3_LIBRARIES} ${LIBSERIALPORT_LIBRARIES})
endif ()

# macOS frameworks: CoreMIDI (recorder trigger) + CoreAudio (24ch capture)
if (APPLE)
    target_link_libraries(${APP_NAME} "-framework CoreMIDI" "-framework CoreAudio"
            "-framework CoreFoundation")
endif ()

# --- Embedded JUCE plugin host (VST/AU) -------------------------------------
# Static C++/Obj-C++ library exposing a flat extern "C" API to the C app.
# Lives in src/host/ which is intentionally NOT covered by the GLOB above, so
# its .mm/.cpp never land in the C target.
if (APPLE)
    include(FetchJUCE)

    add_library(m8c_juce_host STATIC src/host/juce_host.mm)
    set_target_properties(m8c_juce_host PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

    target_compile_definitions(m8c_juce_host PRIVATE
            JUCE_STANDALONE_APPLICATION=1
            JUCE_MODAL_LOOPS_PERMITTED=1
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_PLUGINHOST_VST3=1
            JUCE_PLUGINHOST_AU=1)

    target_link_libraries(m8c_juce_host PRIVATE
            juce::juce_core
            juce::juce_events
            juce::juce_data_structures
            juce::juce_audio_basics
            juce::juce_audio_processors
            juce::juce_audio_formats
            juce::juce_gui_basics
            juce::juce_gui_extra
            juce::juce_dsp
            juce::juce_recommended_config_flags)

    # macOS frameworks JUCE's audio-plugin + AU-GUI hosting needs.
    target_link_libraries(m8c_juce_host PUBLIC
            "-framework Cocoa"
            "-framework CoreAudio"
            "-framework CoreAudioKit"
            "-framework AudioToolbox"
            "-framework AudioUnit"
            "-framework CoreMIDI"
            "-framework QuartzCore"
            "-framework Accelerate"
            "-framework Metal"
            "-framework IOKit")

    target_include_directories(${APP_NAME} PRIVATE src/host)
    target_link_libraries(${APP_NAME} m8c_juce_host)
endif ()

if (APPLE)
    # Destination paths below are relative to ${CMAKE_INSTALL_PREFIX}
    install(TARGETS ${APP_NAME}
            BUNDLE DESTINATION . COMPONENT Runtime
            RUNTIME DESTINATION bin COMPONENT Runtime
    )

    set(CODESIGN_CERT_NAME "" CACHE STRING "The developer ID cert name for codesign")

    set_target_properties(${APP_NAME} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/package/macos/m8c.app/Contents/Info.plist"
            MACOSX_BUNDLE_BUNDLE_NAME "m8c"
            MACOSX_BUNDLE_COPYRIGHT "Copyright © 2025 laamaa. All rights reserved."
            MACOSX_BUNDLE_GUI_IDENTIFIER "fi.laamaa.m8c"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_ICON_FILE "m8c.icns"
    )

    set(APPS "\${CMAKE_INSTALL_PREFIX}/${APP_NAME}.app")

    if(CODESIGN_CERT_NAME)
        # Use the specified certificate
        install(CODE "include(BundleUtilities)
        fixup_bundle(\"${APPS}\" \"\" \"\")
        execute_process(COMMAND codesign --force --options runtime --deep --entitlements ../package/macos/Entitlements.plist --sign \"${CODESIGN_CERT_NAME}\" --timestamp  \${CMAKE_INSTALL_PREFIX}/${APP_NAME}.app)")
    else()
        # Use ad-hoc signing (self-signed) when no certificate is specified
        install(CODE "include(BundleUtilities)
        fixup_bundle(\"${APPS}\" \"\" \"\")
        execute_process(COMMAND codesign --force --options runtime --deep --entitlements ../package/macos/Entitlements.plist --sign - --timestamp  \${CMAKE_INSTALL_PREFIX}/${APP_NAME}.app)")
    endif()

    set(CPACK_PACKAGE_NAME "m8c")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_DIRECTORY "package-output")
    set(CPACK_GENERATOR "DragNDrop")
    include(CPack)
elseif (UNIX)
    install(TARGETS ${APP_NAME})
    install(DIRECTORY package/share/applications DESTINATION share/)
    install(DIRECTORY package/share/icons DESTINATION share/)
    install(FILES AUDIOGUIDE.md README.md LICENSE DESTINATION share/doc/${APP_NAME})
    install(FILES gamecontrollerdb.txt DESTINATION bin/)
endif ()
