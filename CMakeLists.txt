cmake_minimum_required(VERSION 3.16)
project(pseyosc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# psmoveapi, built here as a static library from its core sources only: its own CMakeLists also builds the
# OpenCV tracker, libusb, the examples and utilities, and changes global compiler flags.
set(PSMOVEAPI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/psmoveapi)
if(NOT EXISTS ${PSMOVEAPI_DIR}/external/hidapi/hidapi/hidapi.h)
    message(FATAL_ERROR "hidapi not found, run 'git submodule update --init --recursive'")
endif()

set(PSMOVEAPI_VERSION_MAJOR 4)
set(PSMOVEAPI_VERSION_MINOR 0)
set(PSMOVEAPI_VERSION_PATCH 12)
if(WIN32)
    set(PSMOVE_USE_PS3EYE_DRIVER ON) # the tracker's camera driver on Windows, see below
endif()
configure_file(${PSMOVEAPI_DIR}/include/psmove_config.h.in ${CMAKE_BINARY_DIR}/psmoveapi/psmove_config.h @ONLY)

file(GLOB PSMOVEAPI_MATH_SRC ${PSMOVEAPI_DIR}/src/math/*.c ${PSMOVEAPI_DIR}/src/math/*.cpp)
add_library(psmoveapi STATIC
    ${PSMOVEAPI_DIR}/src/psmove.c
    ${PSMOVEAPI_DIR}/src/psmove_calibration.c
    ${PSMOVEAPI_DIR}/src/psmove_orientation.cpp
    ${PSMOVEAPI_DIR}/src/daemon/moved_client.cpp
    ${PSMOVEAPI_MATH_SRC})
target_include_directories(psmoveapi
    PUBLIC
        ${PSMOVEAPI_DIR}/include
        ${PSMOVEAPI_DIR}/src # private headers: psmove_private.h, psmove_port.h
        ${PSMOVEAPI_DIR}/external/hidapi/hidapi
        ${CMAKE_BINARY_DIR}/psmoveapi
    PRIVATE
        ${PSMOVEAPI_DIR}/external/glm)
# PSMOVE_STATIC is a local patch of psmove.h: without it, the header declares dllimport on Windows.
target_compile_definitions(psmoveapi PUBLIC PSMOVE_STATIC)
if(WIN32)
    target_sources(psmoveapi PRIVATE
        ${PSMOVEAPI_DIR}/external/hidapi/windows/hid.c
        ${PSMOVEAPI_DIR}/src/platform/psmove_port_windows.c)
    target_compile_definitions(psmoveapi PRIVATE WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES NOMINMAX
                                                 _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
    target_link_libraries(psmoveapi PUBLIC setupapi bthprops ws2_32)
    if(MSVC)
        target_compile_options(psmoveapi PRIVATE /Zc:preprocessor /W1)
    endif()
elseif(APPLE)
    enable_language(OBJCXX)
    target_sources(psmoveapi PRIVATE
        ${PSMOVEAPI_DIR}/external/hidapi/mac/hid.c
        ${PSMOVEAPI_DIR}/src/platform/psmove_port_osx.mm)
    target_link_libraries(psmoveapi PUBLIC "-framework Foundation" "-framework IOKit" "-framework CoreFoundation"
                                           "-framework IOBluetooth")
else()
    find_package(PkgConfig REQUIRED)
    find_package(Threads REQUIRED)
    pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
    pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
    pkg_check_modules(BLUEZ REQUIRED IMPORTED_TARGET bluez>=5)
    target_sources(psmoveapi PRIVATE
        ${PSMOVEAPI_DIR}/external/hidapi/linux/hid.c
        ${PSMOVEAPI_DIR}/src/platform/psmove_port_linux.cpp)
    target_compile_definitions(psmoveapi PRIVATE _GNU_SOURCE)
    target_link_libraries(psmoveapi PUBLIC PkgConfig::DBUS PkgConfig::UDEV PkgConfig::BLUEZ Threads::Threads m)
endif()

add_library(hub STATIC src/hub/MoveHub.h src/hub/MoveHub.cpp src/hub/BluetoothLink.h)
target_include_directories(hub PUBLIC src)
target_link_libraries(hub PUBLIC psmoveapi)
if(WIN32)
    target_sources(hub PRIVATE src/hub/BluetoothLink_win.cpp)
    # psmove_port.h includes windows.h
    target_compile_definitions(hub PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_sources(hub PRIVATE src/hub/BluetoothLink_other.cpp)
endif()

add_executable(pseyosc-pair src/pairtool/main.cpp)
target_link_libraries(pseyosc-pair PRIVATE hub)

add_library(osc STATIC src/osc/OscSender.h src/osc/OscSender.cpp src/osc/OscReceiver.h src/osc/OscReceiver.cpp)
target_include_directories(osc PUBLIC src)
if(WIN32)
    target_link_libraries(osc PRIVATE ws2_32)
endif()

# xioTechnologies Fusion (Madgwick's AHRS), C library; the sensor fusion is taken from wiimosc
add_subdirectory(external/Fusion/Fusion ${CMAKE_BINARY_DIR}/external/Fusion EXCLUDE_FROM_ALL)

add_library(motionfusion STATIC
    src/fusion/Orientation.h
    src/fusion/Orientation.cpp
    src/fusion/AbstractOrientationFilter.h
    src/fusion/MadgwickFilter.h
    src/fusion/MadgwickFilter.cpp
    src/fusion/MotionFusion.h
    src/fusion/MotionFusion.cpp)
target_include_directories(motionfusion PUBLIC src)
target_link_libraries(motionfusion PRIVATE Fusion)
# Fusion's headers use C99 designated initializers, which C++ only has since C++20. Only this target's sources
# include them; its own headers stay C++17.
set_target_properties(motionfusion PROPERTIES CXX_STANDARD 20)

add_executable(pseyosc-stream src/stream/main.cpp src/stream/MoveStreamer.h src/stream/MoveStreamer.cpp)
target_link_libraries(pseyosc-stream PRIVATE hub osc motionfusion)
if(WIN32)
    target_compile_definitions(pseyosc-stream PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Camera: PS3EYEDriver talks to the PS3 Eye directly over libusb (psmoveapi's camera driver on Windows). Both are
# psmoveapi submodules; libusb is built from source like hidapi. Windows only for now: elsewhere, psmoveapi reads the
# camera with V4L2 (Linux) or AVFoundation (macOS), and the viewer's window is Win32.
if(WIN32)
    set(LIBUSB_DIR ${PSMOVEAPI_DIR}/external/libusb-1.0)
    set(PS3EYEDRIVER_DIR ${PSMOVEAPI_DIR}/external/PS3EYEDriver)
    if(NOT EXISTS ${LIBUSB_DIR}/libusb/libusb.h OR NOT EXISTS ${PS3EYEDRIVER_DIR}/src/ps3eye.h)
        message(FATAL_ERROR "libusb or PS3EYEDriver not found, run 'git submodule update --init --recursive'")
    endif()

    # The sources of libusb's msvc/libusb_static.vcxproj, without hotplug support.
    add_library(libusb STATIC
        ${LIBUSB_DIR}/libusb/core.c
        ${LIBUSB_DIR}/libusb/descriptor.c
        ${LIBUSB_DIR}/libusb/hotplug.c
        ${LIBUSB_DIR}/libusb/io.c
        ${LIBUSB_DIR}/libusb/strerror.c
        ${LIBUSB_DIR}/libusb/sync.c
        ${LIBUSB_DIR}/libusb/os/events_windows.c
        ${LIBUSB_DIR}/libusb/os/threads_windows.c
        ${LIBUSB_DIR}/libusb/os/windows_common.c
        ${LIBUSB_DIR}/libusb/os/windows_usbdk.c
        ${LIBUSB_DIR}/libusb/os/windows_winusb.c)
    target_include_directories(libusb PUBLIC ${LIBUSB_DIR}/libusb PRIVATE ${LIBUSB_DIR}/msvc)
    target_compile_definitions(libusb PRIVATE _WIN32_WINNT=_WIN32_WINNT_VISTA _CRT_SECURE_NO_WARNINGS)

    # ps3eye_capi: the C API psmoveapi's camera driver uses.
    add_library(ps3eye STATIC ${PS3EYEDRIVER_DIR}/src/ps3eye.h ${PS3EYEDRIVER_DIR}/src/ps3eye.cpp
                              ${PS3EYEDRIVER_DIR}/src/ps3eye_capi.h ${PS3EYEDRIVER_DIR}/src/ps3eye_capi.cpp)
    target_include_directories(ps3eye PUBLIC ${PS3EYEDRIVER_DIR}/src)
    target_link_libraries(ps3eye PUBLIC libusb)
    if(MSVC)
        # As in psmoveapi: PS3EYEDriver uses timeval, which newer libusb headers no longer provide.
        target_compile_options(ps3eye PRIVATE /FIwinsock2.h /W1)
    endif()

    add_executable(pseyosc-video src/video/main.cpp)
    target_link_libraries(pseyosc-video PRIVATE ps3eye)
    target_compile_definitions(pseyosc-video PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    # psmoveapi's tracker needs OpenCV 4. It's looked for only in PSEYOSC_OPENCV_DIR (an OpenCV install folder,
    # containing OpenCVConfig.cmake), never in system locations or OpenCV_DIR, so a stray OpenCV isn't picked up.
    # By default that's the workspace's lib/opencv/install, built by lib/opencv/build.sh. Without it, pseyosc-track
    # isn't built.
    set(PSEYOSC_OPENCV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../lib/opencv/install CACHE PATH "OpenCV 4 install folder")
    unset(OpenCV_DIR CACHE) # otherwise a previous result would win over a changed PSEYOSC_OPENCV_DIR
    set(OpenCV_STATIC ON) # lib/opencv builds static libraries; OpenCV's Windows config looks for DLLs otherwise
    find_package(OpenCV 4 CONFIG QUIET COMPONENTS core imgproc calib3d highgui videoio
                 PATHS ${PSEYOSC_OPENCV_DIR} NO_DEFAULT_PATH)
    if(OpenCV_FOUND)
        message(STATUS "OpenCV ${OpenCV_VERSION}: ${OpenCV_DIR}")
        set(PSMOVEAPI_TRACKER_DIR ${PSMOVEAPI_DIR}/src/tracker)
        add_library(psmoveapi_tracker STATIC
            ${PSMOVEAPI_TRACKER_DIR}/psmove_tracker.cpp
            ${PSMOVEAPI_TRACKER_DIR}/psmove_tracker_hue_calibration.cpp
            ${PSMOVEAPI_TRACKER_DIR}/tracker_helpers.cpp
            ${PSMOVEAPI_TRACKER_DIR}/camera_control.cpp
            ${PSMOVEAPI_TRACKER_DIR}/camera_control_driver.cpp
            ${PSMOVEAPI_TRACKER_DIR}/camera_control_layouts.cpp
            ${PSMOVEAPI_TRACKER_DIR}/camera_control_driver_ps3eyedriver.cpp
            ${PSMOVEAPI_TRACKER_DIR}/psmove_fusion.cpp)
        target_include_directories(psmoveapi_tracker PRIVATE ${PSMOVEAPI_DIR}/external/glm)
        target_link_libraries(psmoveapi_tracker PUBLIC psmoveapi ps3eye ${OpenCV_LIBS})
        target_compile_definitions(psmoveapi_tracker PRIVATE WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES NOMINMAX
                                                             _CRT_SECURE_NO_DEPRECATE)
        if(MSVC)
            # C4244: psmove_tracker.cpp stores double HSV values as floats.
            target_compile_options(psmoveapi_tracker PRIVATE /Zc:preprocessor /W1 /wd4244)
        endif()

        add_executable(pseyosc-track src/track/main.cpp)
        target_link_libraries(pseyosc-track PRIVATE psmoveapi_tracker)
    else()
        message(WARNING "OpenCV 4 not found in PSEYOSC_OPENCV_DIR (${PSEYOSC_OPENCV_DIR}): pseyosc-track and "
                        "pseyosc-stream's --track aren't built. Build it with lib/opencv/build.sh, or set "
                        "PSEYOSC_OPENCV_DIR to an OpenCV install folder.")
    endif()
endif()

# Camera tracking for pseyosc-stream (--track); without psmoveapi's tracker, a stub whose start() fails.
if(TARGET psmoveapi_tracker)
    add_library(tracking STATIC src/tracking/MoveTracker.h src/tracking/MoveTracker.cpp)
    target_link_libraries(tracking PRIVATE psmoveapi_tracker)
else()
    add_library(tracking STATIC src/tracking/MoveTracker.h src/tracking/MoveTracker_none.cpp)
endif()
target_include_directories(tracking PUBLIC src)
target_link_libraries(tracking PUBLIC hub)
if(WIN32)
    target_compile_definitions(tracking PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_link_libraries(pseyosc-stream PRIVATE tracking)
