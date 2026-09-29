cmake_minimum_required(VERSION 3.20)
project(AOI VERSION 1.0.0 LANGUAGES C CXX)

# AOI host: one self-contained, statically linked Windows console executable.
# Dependencies are built from source into third_party/install (see build.ps1).

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(TP "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
set(TPI "${TP}/install")
set(MINGW_ROOT "C:/msys64/mingw64" CACHE PATH "MinGW-w64 root (OpenSSL, zlib)")

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

set(SOURCES
  src/main.cpp
  src/cli.cpp
  src/util.cpp
  src/crypto.cpp
  src/http.cpp
  src/rendezvous.cpp
  src/dsp.cpp
  src/audio.cpp
  src/rtp.cpp
  src/cc.cpp
  src/netsim.cpp
  src/talkback.cpp
  src/listener.cpp
  src/engine.cpp
  src/control.cpp
  src/install.cpp
  src/selftest.cpp
  res/app.rc
)

add_executable(aoi ${SOURCES})
target_include_directories(aoi PRIVATE
  src
  ${TPI}/include
  ${TPI}/include/opus
  ${TP}/libdatachannel/deps/json/single_include
  ${MINGW_ROOT}/include)
target_compile_definitions(aoi PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00
  RTC_STATIC __USE_MINGW_ANSI_STDIO=1 AOI_VERSION="${PROJECT_VERSION}")
target_compile_options(aoi PRIVATE -O2 -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers)

target_link_directories(aoi PRIVATE ${TPI}/lib ${MINGW_ROOT}/lib)
target_link_libraries(aoi PRIVATE
  speexresample qrcodegen
  datachannel juice srtp2 usrsctp
  opus
  ${MINGW_ROOT}/lib/libssl.a ${MINGW_ROOT}/lib/libcrypto.a ${MINGW_ROOT}/lib/libz.a
  winhttp ws2_32 iphlpapi bcrypt crypt32 secur32
  ole32 oleaut32 uuid avrt ksuser
  shell32 shlwapi advapi32 user32 winmm)
target_link_options(aoi PRIVATE -static -static-libgcc -static-libstdc++ -s -municode)
