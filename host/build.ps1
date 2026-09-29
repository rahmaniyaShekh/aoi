# Builds aoi.exe (static, single file) with MSYS2 MinGW-w64 GCC.
#
#   powershell -ExecutionPolicy Bypass -File host\build.ps1
#
# Needs: C:\msys64 with mingw-w64-x86_64-gcc, -openssl, -zlib (pacman), and
# the dependency sources/builds under third_party (see README "Building").
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $root '.tools'
$env:PATH = "C:\msys64\mingw64\bin;$tools\cmake-3.30.5-windows-x86_64\bin;$tools;$env:PATH"

python "$PSScriptRoot\res\make_icon.py" | Out-Null
$build = Join-Path $PSScriptRoot 'build'
cmake -S $PSScriptRoot -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_RC_COMPILER=windres | Out-Null
cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "build failed" }
$exe = Join-Path $build 'aoi.exe'
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item $exe (Join-Path $dist 'aoi.exe') -Force
"built {0} ({1:N1} MB)" -f (Join-Path $dist 'aoi.exe'), ((Get-Item $exe).Length / 1MB)
