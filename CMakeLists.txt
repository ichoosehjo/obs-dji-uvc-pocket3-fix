cmake_minimum_required(VERSION 3.22)
project(obs-dji-uvc VERSION 1.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# obs-dji-uvc — DJI Osmo Pocket encoded-UVC source for OBS Studio.
# See README.md.  Dependencies:
#   * libobs  — via find_package(libobs) or -DLIBOBS_INCLUDE_DIR/-DLIBOBS_LIB
#   * FFmpeg  — from obs-deps (cmake/FindFFmpeg.cmake)
#   * libusb  — FetchContent (libusb-cmake, builds clean under MSVC)
#   * libuvc  — FetchContent, sources vendored directly (its own CMake is
#               not reliable on Windows CI, so we compile its .c files)
# ---------------------------------------------------------------------------

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FetchContent)

# --- libobs ------------------------------------------------------------------
find_package(libobs QUIET)
if(NOT libobs_FOUND)
  if(NOT LIBOBS_INCLUDE_DIR OR NOT LIBOBS_LIB)
    message(FATAL_ERROR
      "libobs not found. Pass -DCMAKE_PREFIX_PATH to an OBS install, or set "
      "-DLIBOBS_INCLUDE_DIR and -DLIBOBS_LIB.")
  endif()
endif()

# --- FFmpeg (obs-deps) ---------------------------------------------------------
find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil swscale)

# --- libusb --------------------------------------------------------------------
FetchContent_Declare(libusb
  GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
  GIT_TAG v1.0.29-0
  GIT_SHALLOW TRUE)
set(LIBUSB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBUSB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LIBUSB_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libusb)

# --- libuvc (vendored sources, bypass its CMake) --------------------------------
FetchContent_Declare(libuvc_src
  GIT_REPOSITORY https://github.com/libuvc/libuvc.git
  GIT_TAG 047920bcdfb1dac42424c90de5cc77dfc9fba04d
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR cmake-disabled-no-add-subdirectory)
FetchContent_MakeAvailable(libuvc_src)

# FIX for MSVC: upstream libuvc.h includes <sys/time.h> unconditionally,
# which does not exist under MSVC.  Patch it in the populated source.
file(READ "${libuvc_src_SOURCE_DIR}/include/libuvc/libuvc.h" _uvc_hdr)
string(FIND "${_uvc_hdr}" "#include <sys/time.h>" _uvc_needs_patch)
if(NOT _uvc_needs_patch EQUAL -1)
  string(REPLACE "#include <sys/time.h>"
"#ifdef _MSC_VER
#include <winsock2.h>  /* struct timeval */
#include <time.h>      /* struct timespec */
#else
#include <sys/time.h>
#endif"
    _uvc_hdr "${_uvc_hdr}")
  file(WRITE "${libuvc_src_SOURCE_DIR}/include/libuvc/libuvc.h" "${_uvc_hdr}")
endif()

# MSVC lacks clock_gettime/CLOCK_MONOTONIC; use C11 timespec_get instead.
file(READ "${libuvc_src_SOURCE_DIR}/src/stream.c" _uvc_stream)
string(FIND "${_uvc_stream}" "timespec_get" _uvc_stream_patched)
if(_uvc_stream_patched EQUAL -1)
  string(REPLACE
"  (void)clock_gettime(CLOCK_MONOTONIC, &strmh->capture_time_finished);"
"#ifdef _MSC_VER
  (void)timespec_get(&strmh->capture_time_finished, TIME_UTC);
#else
  (void)clock_gettime(CLOCK_MONOTONIC, &strmh->capture_time_finished);
#endif"
    _uvc_stream "${_uvc_stream}")
  file(WRITE "${libuvc_src_SOURCE_DIR}/src/stream.c" "${_uvc_stream}")
endif()

# Generate libuvc_config.h (no JPEG — we only consume the H.264 descriptor)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/uvc-config/libuvc/libuvc_config.h" "
#ifndef LIBUVC_CONFIG_H
#define LIBUVC_CONFIG_H
#define LIBUVC_VERSION_MAJOR 0
#define LIBUVC_VERSION_MINOR 0
#define LIBUVC_VERSION_PATCH 7
#define LIBUVC_VERSION_STR \"0.0.7\"
#define LIBUVC_VERSION_INT ((0 << 16) | (0 << 8) | 7)
/* #undef LIBUVC_HAS_JPEG */
#endif
")

add_library(uvc_vendored STATIC
  ${libuvc_src_SOURCE_DIR}/src/ctrl.c
  ${libuvc_src_SOURCE_DIR}/src/ctrl-gen.c
  ${libuvc_src_SOURCE_DIR}/src/device.c
  ${libuvc_src_SOURCE_DIR}/src/diag.c
  ${libuvc_src_SOURCE_DIR}/src/frame.c
  ${libuvc_src_SOURCE_DIR}/src/init.c
  ${libuvc_src_SOURCE_DIR}/src/misc.c
  ${libuvc_src_SOURCE_DIR}/src/stream.c)
target_include_directories(uvc_vendored PUBLIC
  ${libuvc_src_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}/uvc-config)
target_link_libraries(uvc_vendored PUBLIC usb-1.0)
if(MSVC)
  target_compile_definitions(uvc_vendored PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(WIN32)
  if(TARGET OBS::w32-pthreads)
    target_link_libraries(uvc_vendored PUBLIC OBS::w32-pthreads)
  elseif(LIBOBS_PTHREADS_LIB)
    target_link_libraries(uvc_vendored PUBLIC ${LIBOBS_PTHREADS_LIB})
    target_include_directories(uvc_vendored PUBLIC ${LIBOBS_PTHREADS_INCLUDE_DIR})
  else()
    message(FATAL_ERROR "No pthreads implementation for libuvc on Windows "
      "(need OBS::w32-pthreads or LIBOBS_PTHREADS_LIB)")
  endif()
endif()

# --- plugin ----------------------------------------------------------------------
add_library(obs-dji-uvc MODULE
  src/plugin-main.c
  src/dji-source.c
  src/dji-capture.c
  src/dji-decode.c
  src/dji-nal.c)
if(WIN32)
  target_sources(obs-dji-uvc PRIVATE src/dji-capture-mf.cpp)
  target_link_libraries(obs-dji-uvc PRIVATE mfplat mf mfreadwrite mfuuid ole32)
  set_property(TARGET obs-dji-uvc PROPERTY CXX_STANDARD 17)
endif()

target_compile_definitions(obs-dji-uvc PRIVATE DJI_HAVE_OBS)

if(libobs_FOUND)
  target_link_libraries(obs-dji-uvc PRIVATE OBS::libobs)
  if(TARGET OBS::w32-pthreads)
    target_link_libraries(obs-dji-uvc PRIVATE OBS::w32-pthreads)
  endif()
else()
  target_include_directories(obs-dji-uvc PRIVATE ${LIBOBS_INCLUDE_DIR})
  if(LIBOBS_CONFIG_INCLUDE_DIR)
    target_include_directories(obs-dji-uvc PRIVATE ${LIBOBS_CONFIG_INCLUDE_DIR})
  endif()
  target_link_libraries(obs-dji-uvc PRIVATE ${LIBOBS_LIB})
  if(LIBOBS_PTHREADS_LIB)
    target_link_libraries(obs-dji-uvc PRIVATE ${LIBOBS_PTHREADS_LIB})
  endif()
  if(LIBOBS_PTHREADS_INCLUDE_DIR)
    target_include_directories(obs-dji-uvc PRIVATE ${LIBOBS_PTHREADS_INCLUDE_DIR})
  endif()
endif()

target_link_libraries(obs-dji-uvc PRIVATE
  FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale uvc_vendored)

find_path(SIMDE_INCLUDE_DIR simde/x86/sse2.h)
if(SIMDE_INCLUDE_DIR)
  target_include_directories(obs-dji-uvc PRIVATE ${SIMDE_INCLUDE_DIR})
endif()

if(WIN32)
  target_compile_definitions(obs-dji-uvc PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set_target_properties(obs-dji-uvc PROPERTIES PREFIX "")

# --- host-side unit test ------------------------------------------------------------
option(DJI_UVC_BUILD_TESTS "Build NAL assembler unit test" OFF)
if(DJI_UVC_BUILD_TESTS)
  add_executable(test-nal src/test-nal.c src/dji-nal.c)
  enable_testing()
  add_test(NAME nal COMMAND test-nal)
endif()

# --- install --------------------------------------------------------------------------
if(WIN32)
  install(TARGETS obs-dji-uvc
          LIBRARY DESTINATION "obs-plugins/64bit"
          RUNTIME DESTINATION "obs-plugins/64bit")
  install(DIRECTORY data/ DESTINATION "data/obs-plugins/obs-dji-uvc")
else()
  install(TARGETS obs-dji-uvc LIBRARY DESTINATION "lib/obs-plugins")
  install(DIRECTORY data/ DESTINATION "share/obs/obs-plugins/obs-dji-uvc")
endif()
