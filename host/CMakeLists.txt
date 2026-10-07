cmake_minimum_required(VERSION 3.20)
project(AOI VERSION 1.0.3 LANGUAGES C CXX)

# AOI host: one self-contained, statically linked console executable.
# Dependencies are built from source into third_party/install (see deps.ps1).
#
#   src/core/          platform-neutral: DSP, voice processing, RTP, congestion
#                      control, listeners, engine, crypto, rendezvous, CLI, selftest
#   src/platform/*.h   the only way core reaches the OS (audio, http, ipc, installer, os)
#   src/platform/win/  WASAPI, WinHTTP, named pipes, per-user install, console
#   src/platform/mac/  compile-gated stub with the same interfaces

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(TP "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
set(TPI "${TP}/install")
if(WIN32)
  set(MINGW_ROOT "C:/msys64/mingw64" CACHE PATH "MinGW-w64 root (OpenSSL, zlib)")
  set(SYS_INCLUDE ${MINGW_ROOT}/include)
  set(SYS_LIB ${MINGW_ROOT}/lib)
endif()

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
  src/core/rtp.cpp
  src/core/cc.cpp
  src/core/netsim.cpp
  src/core/talkback.cpp
  src/core/listener.cpp
  src/core/engine.cpp
  src/core/control.cpp
  src/core/cli.cpp
  src/core/selftest.cpp
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
if(WIN32)
  target_compile_definitions(aoi_core PUBLIC __USE_MINGW_ANSI_STDIO=1)
endif()
target_compile_options(aoi_core PRIVATE -O2 -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers)
target_link_libraries(aoi_core PUBLIC speexresample qrcodegen)

# ---- platform ---------------------------------------------------------------------------
if(WIN32)
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
elseif(APPLE)
  # Compile-gated stub: builds, runs `aoi selftest`, and says "not supported"
  # for everything that needs the OS. See src/platform/mac/README.md.
  add_executable(aoi
    src/platform/mac/main_mac.cpp
    src/platform/mac/os_mac.cpp
    src/platform/mac/audio_mac.cpp
    src/platform/mac/net_mac.cpp)
  target_link_directories(aoi PRIVATE ${TPI}/lib)
  target_link_libraries(aoi PRIVATE aoi_core datachannel juice srtp2 usrsctp opus ssl crypto z)
else()
  message(FATAL_ERROR "AOI builds for Windows (and a macOS stub) only")
endif()
target_compile_options(aoi PRIVATE -O2 -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers)
