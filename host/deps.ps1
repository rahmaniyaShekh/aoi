# Fetches and builds AOI's dependencies (static) into third_party/install.
#
#   powershell -ExecutionPolicy Bypass -File host\deps.ps1
#   powershell -ExecutionPolicy Bypass -File host\build.ps1
#
# Needs MSYS2 at C:\msys64 with:  pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-openssl mingw-w64-x86_64-zlib
# CMake and Ninja are downloaded (portable) into .tools. Versions are pinned.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tp = Join-Path $root 'third_party'
$tools = Join-Path $root '.tools'
$cmakeDir = Join-Path $tools 'cmake-3.30.5-windows-x86_64'
New-Item -ItemType Directory -Force $tp, $tools | Out-Null
$ProgressPreference = 'SilentlyContinue'

if (-not (Test-Path "$cmakeDir\bin\cmake.exe")) {
  Invoke-WebRequest -UseBasicParsing https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-windows-x86_64.zip -OutFile "$tools\cmake.zip"
  Expand-Archive "$tools\cmake.zip" $tools -Force
}
if (-not (Test-Path "$tools\ninja.exe")) {
  Invoke-WebRequest -UseBasicParsing https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip -OutFile "$tools\ninja.zip"
  Expand-Archive "$tools\ninja.zip" $tools -Force
}
$env:PATH = "C:\msys64\mingw64\bin;$cmakeDir\bin;$tools;$env:PATH"

function Get-Source($name, $url, $ref, [switch]$Recursive) {
  $dir = Join-Path $tp $name
  if (Test-Path $dir) { return }
  if ($ref -match '^v') {
    $args = @('-c', 'advice.detachedHead=false', 'clone', '-q', '--depth', '1', '--branch', $ref)
    if ($Recursive) { $args += @('--recurse-submodules', '--shallow-submodules') }
    git @args $url $dir
  } else {
    git clone -q $url $dir
    git -C $dir -c advice.detachedHead=false checkout -q $ref
  }
  if ($LASTEXITCODE -ne 0) { throw "could not fetch $name" }
}

Get-Source 'opus' 'https://github.com/xiph/opus.git' 'v1.6.1'
Get-Source 'libdatachannel' 'https://github.com/paullouisageneau/libdatachannel.git' 'v0.24.6' -Recursive
Get-Source 'speexdsp' 'https://github.com/xiph/speexdsp.git' '7a158783df74efe7c2d1c6ee8363c1e695c71226'
Get-Source 'qrcodegen' 'https://github.com/nayuki/QR-Code-generator.git' '3c6d0b3cefb4e049dc337e82237c9644399716a8'

$prefix = (Join-Path $tp 'install') -replace '\\', '/'
$common = @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=gcc', '-DCMAKE_CXX_COMPILER=g++', "-DCMAKE_INSTALL_PREFIX=$prefix")

cmake -S "$tp\opus" -B "$tp\build-opus" @common -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF | Out-Null
cmake --build "$tp\build-opus"; cmake --install "$tp\build-opus" | Out-Null

# libsrtp's warnings-as-errors trips over MinGW's printf; the code is fine.
cmake -S "$tp\libdatachannel" -B "$tp\build-dc" @common -DBUILD_SHARED_LIBS=OFF -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON `
  -DENABLE_WARNINGS_AS_ERRORS=OFF -DLIBSRTP_TEST_APPS=OFF "-DCMAKE_C_FLAGS=-D__USE_MINGW_ANSI_STDIO=1" `
  -DOPENSSL_USE_STATIC_LIBS=TRUE -DOPENSSL_ROOT_DIR=C:/msys64/mingw64 | Out-Null
cmake --build "$tp\build-dc"; cmake --install "$tp\build-dc" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'dependency build failed' }
"dependencies ready in $prefix"
