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

add_executable(pseyosc-stream src/stream/main.cpp src/stream/MoveStreamer.h src/stream/MoveStreamer.cpp)
target_link_libraries(pseyosc-stream PRIVATE hub osc)
if(WIN32)
    target_compile_definitions(pseyosc-stream PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
