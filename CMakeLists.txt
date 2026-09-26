cmake_minimum_required(VERSION 3.16)

# Use the RISC-V cross-compiler toolchain
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/toolchain-app.cmake")
endif()

project(app C)

# App source files — add your .c files here
set(APP_SOURCES
    main/main.c
    main/crt0.c
    main/nfm/netraw.c
    main/nfm/usbnet.c
    main/nfm/ncmtest.c
    main/nfm/pattern.c
    main/nfm/enctest.c
    main/nfm/tsmux.c
    main/nfm/stream.c
)

# The test kit's console half (main/testkit/, README "Automated device
# tests"): the listener and the record format. nfmtest drives its own
# test (RUN ncm, main/nfm/ncmtest.h) rather than the kit's perf/shots.
list(APPEND APP_SOURCES
    main/testkit/debugcon.c
    main/testkit/report.c
    main/testkit/screenshot.c
)
add_compile_definitions(REPORT_PREFIX="NFM" TESTKIT_NO_ENGINE SCREENSHOT_DIR="/sd/nfmtest")

# Build identity for the debug console's READY records: the git commit
# (marked -dirty when there are uncommitted changes) and the build time.
# `make build` re-runs cmake, so this is fresh on every build. Without it
# the app reports "unknown" and tools/testrun.py refuses to believe any
# result, on the grounds that it cannot tell which build produced it.
execute_process(
    COMMAND git describe --always --dirty --abbrev=10
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE APP_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT APP_GIT_HASH)
    set(APP_GIT_HASH "unknown")
endif()
string(TIMESTAMP APP_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)
configure_file(main/app_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/app_version.h @ONLY)

# Include directories — add paths to your headers here
set(APP_INCLUDES
    main
    main/testkit
    components
    components/tinyusb/src
    include
    include/esp_additions/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# TinyUSB, vendored (components/tinyusb/PROVENANCE.md): device core,
# CDC-NCM and the DWC2 port, configured by components/tusb_config.h.
# Compiled into app.so like the app (F-02/F-08), with its own warning set:
# it is upstream code, kept byte-identical.
set(TINYUSB_SOURCES
    components/tinyusb/src/tusb.c
    components/tinyusb/src/common/tusb_fifo.c
    components/tinyusb/src/device/usbd.c
    components/tinyusb/src/class/net/ncm_device.c
    components/tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c
    components/tinyusb/src/portable/synopsys/dwc2/dwc2_common.c
)
list(APPEND APP_SOURCES ${TINYUSB_SOURCES})

set_source_files_properties(${TINYUSB_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-sign-compare;-Wno-type-limits;-Wno-missing-field-initializers")

# Compile app sources as a relocatable object library
add_library(app_obj OBJECT ${APP_SOURCES})
target_include_directories(app_obj PRIVATE ${APP_INCLUDES})
target_compile_options(app_obj PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -Os -fPIC
    -ffunction-sections -fdata-sections -fno-common
    -D__thread=
    -DESP_PLATFORM
)

# ESP-IDF normally force-includes FreeRTOS headers for all components
target_compile_options(app_obj PRIVATE
    "SHELL:-include freertos/FreeRTOS.h"
    "SHELL:-include freertos/semphr.h"
    "SHELL:-include freertos/task.h"
    "SHELL:-include freertos/queue.h"
    "SHELL:-include freertos/event_groups.h"
)

# SynthEngine3D, the 3D engine (https://github.com/nullislandspace/synthengine3D),
# if this app has it. It is not part of the template: an app that wants 3D adds
# it as a git submodule at synthengine3D/ (`make engine`, see README), and this
# block then builds it. Without it every variable below stays empty and the
# link line is exactly the one a plain app has always had, so apps that do not
# use the engine are unaffected by merges from the template.
#
# In this plain-CMake build the engine is a relocatable OBJECT library, so
# add_subdirectory MUST come before the link script's file(GENERATE) for
# $<TARGET_OBJECTS:synthengine3d> to resolve. Linking it to app_obj propagates
# the engine's PUBLIC include directory (synthengine3D/include), which is what
# lets app sources #include "synthengine3d.h"; its object files are folded into
# app.so by the link script below, since the custom command, not CMake, links.
set(ENGINE_OBJS "")
set(ENGINE_DEPS "")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/synthengine3D/CMakeLists.txt)
    message(STATUS "SynthEngine3D: building synthengine3D/")
    add_subdirectory(synthengine3D)
    target_link_libraries(app_obj PRIVATE synthengine3d)
    set(ENGINE_OBJS "$<JOIN:$<TARGET_OBJECTS:synthengine3d>, >")
    list(APPEND ENGINE_DEPS synthengine3d $<TARGET_OBJECTS:synthengine3d>)
endif()

# Collect object files into a list file, then link via a response file
# This avoids issues with generator expressions in custom commands
set(LINK_SCRIPT ${CMAKE_BINARY_DIR}/link_app.sh)
file(GENERATE OUTPUT ${LINK_SCRIPT} CONTENT
"#!/bin/sh
${CMAKE_C_COMPILER} \
    -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p1 -mabi=ilp32f \
    -shared -nostdlib \
    -Wl,--gc-sections \
    -Wl,-Bsymbolic \
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/app_version.script \
    -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/app.ld \
    -Wl,-Map=${CMAKE_BINARY_DIR}/app.map \
    -L${CMAKE_CURRENT_SOURCE_DIR}/fakelib \
    $<JOIN:$<TARGET_OBJECTS:app_obj>, > ${ENGINE_OBJS} \
    -lall \
    -o ${CMAKE_BINARY_DIR}/app.so
")

add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/app.so
    COMMAND sh ${LINK_SCRIPT}
    DEPENDS app_obj
        $<TARGET_OBJECTS:app_obj>
        ${ENGINE_DEPS}
        ${CMAKE_CURRENT_SOURCE_DIR}/app.ld
        ${CMAKE_CURRENT_SOURCE_DIR}/app_version.script
        ${CMAKE_CURRENT_SOURCE_DIR}/fakelib/liball.so
    COMMENT "Linking app.so"
)

add_custom_target(app ALL DEPENDS ${CMAKE_BINARY_DIR}/app.so)

# Print size after build
add_custom_command(TARGET app POST_BUILD
    COMMAND ${CMAKE_SIZE} ${CMAKE_BINARY_DIR}/app.so
    COMMENT "app.so size:"
)
