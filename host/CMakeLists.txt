cmake_minimum_required(VERSION 3.20)
project(AOI VERSION 1.0.4 LANGUAGES C CXX)

# AOI host for Windows: one self-contained, statically linked console
# executable. Dependencies are built from source into third_party/install
# (see deps.ps1).
#
#   src/core/          platform-neutral algorithms: DSP, voice processing, RTP,
#                      congestion control, listeners, engine, crypto, rendezvous,
#                      CLI, selftest. No OS calls (checked on every build).
#   src/platform/*.h   the only way core reaches the OS (audio, http, ipc, installer, os)
#   src/platform/win/  WASAPI, WinHTTP, named pipes, per-user install, console
#
# This project builds Windows only. A macOS host is a separate project (see
# ../mac/README.md); none of its code is compiled here.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(TP "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
set(TPI "${TP}/install")
if(NOT WIN32)
  message(FATAL_ERROR "host/ builds the Windows host only; the macOS host is a separate project (../mac)")
endif()
set(MINGW_ROOT "C:/msys64/mingw64" CACHE PATH "MinGW-w64 root (OpenSSL, zlib)")
set(SYS_INCLUDE ${MINGW_ROOT}/include)
set(SYS_LIB ${MINGW_ROOT}/lib)

# SpeexDSP resampler, compiled straight in (one file, BSD).
add_library(speexresample STATIC ${TP}/speexdsp/libspeexdsp/resample.c)
target_include_directories(speexresample PUBLIC ${TP}/speexdsp/include/speex)
target_compile_definitions(speexresample PUBLIC OUTSIDE_SPEEX RANDOM_PREFIX=aoi)
target_compile_definitions(speexresample PRIVATE FLOATING_POINT USE_SSE USE_SSE2 EXPORT=)
target_compile_options(speexresample PRIVATE -O2 -msse2)

# Terminal QR codes for the join link (Nayuki, MIT).
add_library(qrcodegen STATIC ${TP}/qrcodegen/cpp/qrcodegen.cpp)
target_include_directories(qrcodegen PUBLIC ${TP}/qrcodegen/cpp)
target_compile_options(qrcodegen PRIVATE -O2)

# ---- platform boundary -----------------------------------------------------------------
file(GLOB CORE_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.h)
set(BOUNDARY_H ${CMAKE_CURRENT_BINARY_DIR}/generated/boundary_report.h)
add_custom_command(
  OUTPUT ${BOUNDARY_H}
  COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/src/core -DOUT=${BOUNDARY_H}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_boundary.cmake
  DEPENDS ${CORE_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_boundary.cmake
  COMMENT "Checking the platform boundary (src/core may not include OS headers)")

# ---- core (platform neutral) ------------------------------------------------------------
add_library(aoi_core STATIC
  src/core/util.cpp
  src/core/crypto.cpp
  src/core/rendezvous.cpp
  src/core/dsp.cpp
  src/core/devices.cpp
  src/core/voice.cpp
  src/core/rtp.cpp
  src/core/cc.cpp
  src/core/netsim.cpp
  src/core/talkback.cpp
  src/core/listener.cpp
  src/core/engine.cpp
  src/core/control.cpp
  src/core/cli.cpp
  src/core/selftest.cpp
  src/core/selftest_voice.cpp
  src/core/echotest.cpp
  ${BOUNDARY_H})
target_include_directories(aoi_core PUBLIC
  src
  src/core
  ${CMAKE_CURRENT_BINARY_DIR}/generated
  ${TPI}/include
  ${TPI}/include/opus
  ${TP}/libdatachannel/deps/json/single_include
  ${SYS_INCLUDE})
target_compile_definitions(aoi_core PUBLIC RTC_STATIC AOI_VERSION="${PROJECT_VERSION}")
target_compile_definitions(aoi_core PUBLIC __USE_MINGW_ANSI_STDIO=1)
target_compile_options(aoi_core PRIVATE -O2 -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers)
target_link_libraries(aoi_core PUBLIC speexresample qrcodegen)

# WebRTC Audio Processing (freedesktop webrtc-audio-processing 2.1, BSD-3:
# AEC3, noise suppression, AGC2) and its abseil, built static by deps.ps1.
# voice.cpp also uses the RNN voice detector header from the same pinned
# source tree. Without it the build falls back to the basic voice chain.
set(APM_SRC ${TP}/webrtc-apm/webrtc)
if(EXISTS ${TPI}/lib/libwebrtc-audio-processing-2.a AND EXISTS ${APM_SRC})
  set(APM_OS_DEFS WEBRTC_WIN NOMINMAX _USE_MATH_DEFINES __STDC_FORMAT_MACROS=1)
  set_source_files_properties(src/core/voice.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${APM_SRC}"
    COMPILE_DEFINITIONS "AOI_HAVE_APM=1;WEBRTC_APM_DEBUG_DUMP=0;${APM_OS_DEFS}")
  target_link_directories(aoi_core PUBLIC ${TPI}/lib)
  target_link_libraries(aoi_core PUBLIC
    -Wl,--start-group webrtc-audio-processing-2
    absl_flags absl_log absl_status absl_strings absl_synchronization absl_time absl_debugging absl_hash
    absl_container absl_crc absl_random absl_numeric absl_profiling absl_types absl_base -Wl,--end-group)
  target_link_libraries(aoi_core PUBLIC winmm)  # APM's clock
  message(STATUS "voice processing: WebRTC APM")
else()
  message(WARNING "WebRTC APM not found (run deps.ps1): building the basic voice chain only")
endif()

# ---- platform ---------------------------------------------------------------------------
add_executable(aoi
  src/platform/win/main_win.cpp
  src/platform/win/os_win.cpp
  src/platform/win/audio_win.cpp
  src/platform/win/http_win.cpp
  src/platform/win/ipc_win.cpp
  src/platform/win/installer_win.cpp
  res/app.rc)
target_compile_definitions(aoi PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_link_directories(aoi PRIVATE ${TPI}/lib ${SYS_LIB})
target_link_libraries(aoi PRIVATE
  aoi_core
  datachannel juice srtp2 usrsctp
  opus
  ${SYS_LIB}/libssl.a ${SYS_LIB}/libcrypto.a ${SYS_LIB}/libz.a
  winhttp ws2_32 iphlpapi bcrypt crypt32 secur32
  ole32 oleaut32 uuid avrt ksuser
  shell32 shlwapi advapi32 user32 winmm)
target_link_options(aoi PRIVATE -static -static-libgcc -static-libstdc++ -s -municode)
target_compile_options(aoi PRIVATE -O2 -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers)
