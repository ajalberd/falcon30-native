param(
    [string]$ZigExe = ""
)

$ErrorActionPreference = "Stop"

$recompRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$src        = Join-Path $recompRoot "src"
$compat     = Join-Path $recompRoot "compat"
$build      = Join-Path $recompRoot "build"
$thirdPty   = Join-Path $recompRoot "third_party"
$objDir     = Join-Path $build "obj"
$stbDir     = Join-Path $thirdPty "stb"

# Prefer CMake + Ninja if installed on PATH
$hasCmake = (Get-Command cmake -ErrorAction SilentlyContinue) -ne $null
$hasNinja = (Get-Command ninja -ErrorAction SilentlyContinue) -ne $null
$hasGxx   = (Get-Command g++   -ErrorAction SilentlyContinue) -ne $null
if ($hasCmake -and $hasNinja -and $hasGxx) {
    Write-Host "Building falcon30_native.exe via CMake + Ninja..."
    & cmake -S $recompRoot -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed!" }
    & cmake --build $build --parallel
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed!" }
    Write-Host "CMake build succeeded: $build\falcon30_native.exe"
    return
}

# Otherwise build cleanly with Zig (C++20 x86_64-windows-gnu)
if ([string]::IsNullOrWhiteSpace($ZigExe)) {
    if ($env:ZIG_EXE -and (Test-Path $env:ZIG_EXE)) {
        $ZigExe = $env:ZIG_EXE
    } elseif (Get-Command zig -ErrorAction SilentlyContinue) {
        $ZigExe = (Get-Command zig).Source
    } elseif (Test-Path "C:\tools\zig\zig-x86_64-windows-0.14.1\zig.exe") {
        $ZigExe = "C:\tools\zig\zig-x86_64-windows-0.14.1\zig.exe"
    } else {
        throw "Neither CMake+Ninja+g++ nor Zig compiler found!"
    }
}

New-Item -ItemType Directory -Force -Path $thirdPty, $build, $objDir, $stbDir | Out-Null

# 1. SDL3 (x86_64-w64-mingw32 dev package)
$sdlVer  = "3.4.20"
$sdlDir  = Join-Path $thirdPty "SDL3-$sdlVer"
$sdlRoot = Join-Path $sdlDir "x86_64-w64-mingw32"
if (-not (Test-Path (Join-Path $sdlRoot "lib\libSDL3.dll.a"))) {
    $zipPath = Join-Path $thirdPty "SDL3-devel-$sdlVer-mingw.zip"
    if (-not (Test-Path $zipPath)) {
        $url = "https://github.com/libsdl-org/SDL/releases/download/release-$sdlVer/SDL3-devel-$sdlVer-mingw.zip"
        Write-Host "Downloading SDL3 $sdlVer MinGW dev package..."
        Invoke-WebRequest -Uri $url -OutFile $zipPath -UseBasicParsing
    }
    Write-Host "Extracting SDL3 $sdlVer..."
    Expand-Archive -Path $zipPath -DestinationPath $thirdPty -Force
}

# 2. Dear ImGui v1.93.0
$imguiVer = "1.93.0"
$imguiDir = Join-Path $thirdPty "imgui-$imguiVer"
if (-not (Test-Path (Join-Path $imguiDir "imgui.h"))) {
    $imguiZip = Join-Path $thirdPty "imgui-$imguiVer.zip"
    if (-not (Test-Path $imguiZip)) {
        Write-Host "Downloading Dear ImGui v$imguiVer..."
        Invoke-WebRequest -Uri "https://github.com/ocornut/imgui/archive/refs/tags/v$imguiVer.zip" -OutFile $imguiZip -UseBasicParsing
    }
    Write-Host "Extracting Dear ImGui v$imguiVer..."
    Expand-Archive -Path $imguiZip -DestinationPath $thirdPty -Force
}

# 3. libx86emu
$x86Dir = Join-Path $thirdPty "libx86emu"
if (-not (Test-Path (Join-Path $x86Dir "include\x86emu.h"))) {
    Write-Host "Cloning libx86emu..."
    & git clone --depth 1 https://github.com/wfeldt/libx86emu.git $x86Dir
    if ($LASTEXITCODE -ne 0) { throw "Failed to clone libx86emu" }
}

# 4. Munt libmt32emu 2.8.6
$muntVer = "libmt32emu_2_8_6"
$muntDir = Join-Path $thirdPty "munt-$muntVer"
$mt32Src = Join-Path $muntDir "mt32emu\src"
if (-not (Test-Path (Join-Path $mt32Src "Synth.cpp"))) {
    $muntZip = Join-Path $thirdPty "munt-$muntVer.zip"
    if (-not (Test-Path $muntZip)) {
        Write-Host "Downloading Munt $muntVer..."
        Invoke-WebRequest -Uri "https://github.com/munt/munt/archive/refs/tags/$muntVer.zip" -OutFile $muntZip -UseBasicParsing
    }
    Write-Host "Extracting Munt $muntVer..."
    Expand-Archive -Path $muntZip -DestinationPath $thirdPty -Force
}
# Generate mt32emu/config.h and stage public headers under mt32emu/ (matching CMakeLists.txt)
$mt32CfgDir = Join-Path $build "mt32emu_gen\mt32emu"
New-Item -ItemType Directory -Force -Path $mt32CfgDir | Out-Null
$mt32CfgH = Join-Path $mt32CfgDir "config.h"
@"
#ifndef MT32EMU_CONFIG_H
#define MT32EMU_CONFIG_H
#define MT32EMU_VERSION "2.8.6"
#define MT32EMU_VERSION_MAJOR 2
#define MT32EMU_VERSION_MINOR 8
#define MT32EMU_VERSION_PATCH 6
#define MT32EMU_EXPORTS_TYPE 0
#undef MT32EMU_SHARED
#define MT32EMU_WITH_VERSION_TAGGING 0
#undef MT32EMU_RUNTIME_VERSION_CHECK
#endif
"@ | Set-Content -Encoding ASCII $mt32CfgH
foreach ($hdr in @(
    "mt32emu.h", "globals.h", "Enumerations.h", "Types.h",
    "File.h", "FileStream.h", "MidiStreamParser.h", "ROMInfo.h",
    "SampleRateConverter.h", "Synth.h"
)) {
    Copy-Item -Force (Join-Path $mt32Src $hdr) (Join-Path $mt32CfgDir $hdr)
}

# 5. nlohmann/json v3.11.3
$jsonVer = "3.11.3"
$jsonDir = Join-Path $thirdPty "json-$jsonVer"
$jsonInc = Join-Path $jsonDir "single_include"
if (-not (Test-Path (Join-Path $jsonInc "nlohmann\json.hpp"))) {
    $jsonZip = Join-Path $thirdPty "json-$jsonVer.zip"
    if (-not (Test-Path $jsonZip)) {
        Write-Host "Downloading nlohmann/json v$jsonVer..."
        Invoke-WebRequest -Uri "https://github.com/nlohmann/json/archive/refs/tags/v$jsonVer.zip" -OutFile $jsonZip -UseBasicParsing
    }
    Write-Host "Extracting nlohmann/json v$jsonVer..."
    Expand-Archive -Path $jsonZip -DestinationPath $thirdPty -Force
}

function Compile-Obj([string]$compilerMode, [string]$srcFile, [string]$tag, [string[]]$extraArgs) {
    $name = ($tag + "_" + [System.IO.Path]::GetFileNameWithoutExtension($srcFile)) -replace '[^a-zA-Z0-9_]', '_'
    $obj  = Join-Path $objDir "$name.o"
    if (-not (Test-Path $obj) -or (Get-Item $srcFile).LastWriteTime -gt (Get-Item $obj).LastWriteTime) {
        Write-Host "  [$tag] $(Split-Path -Leaf $srcFile)"
        & $ZigExe $compilerMode -target x86_64-windows-gnu -O2 @extraArgs -c "$srcFile" -o "$obj"
        if ($LASTEXITCODE -ne 0) { throw "Failed to compile $srcFile" }
    }
    return $obj
}

$allObjs = @()

Write-Host "Compiling Dear ImGui..."
$imguiFlags = @("-std=c++20", "-I$sdlRoot\include", "-I$imguiDir", "-I$imguiDir\backends")
foreach ($f in @(
    "$imguiDir\imgui.cpp",
    "$imguiDir\imgui_draw.cpp",
    "$imguiDir\imgui_tables.cpp",
    "$imguiDir\imgui_widgets.cpp",
    "$imguiDir\backends\imgui_impl_sdl3.cpp",
    "$imguiDir\backends\imgui_impl_opengl3.cpp"
)) {
    $allObjs += Compile-Obj "c++" $f "imgui" $imguiFlags
}

Write-Host "Compiling libx86emu..."
$x86Flags = @("-std=gnu11", "-w", "-I$x86Dir\include", "-I$x86Dir", "-I$compat")
$x86Sources = Get-ChildItem "$x86Dir\*.c" | Where-Object { $_.Name -notmatch 'test' -and $_.Name -ne 'prim_ops.c' } | ForEach-Object { $_.FullName }
$x86Sources += "$compat\x86emu_prim_ops.c"
foreach ($f in $x86Sources) {
    $allObjs += Compile-Obj "cc" $f "x86emu" $x86Flags
}

Write-Host "Compiling Munt libmt32emu..."
$mt32Flags = @("-std=c++17", "-w", "-DMT32EMU_WITH_INTERNAL_RESAMPLER=1", "-DMT32EMU_FASTER_MATH=1", "-I$build\mt32emu_gen", "-I$mt32CfgDir", "-I$mt32Src")
$mt32Sources = Get-ChildItem $mt32Src -Recurse -Filter "*.cpp" | Where-Object {
    $_.FullName -notmatch '\\c_interface\\' -and
    $_.FullName -notmatch '\\test\\' -and
    $_.Name -notmatch 'SoxrAdapter|SamplerateAdapter|VersionTagging'
} | ForEach-Object { $_.FullName }
foreach ($f in $mt32Sources) {
    $allObjs += Compile-Obj "c++" $f "mt32" $mt32Flags
}

Write-Host "Compiling Falcon 3.0 Native Host..."
$appFlags = @(
    "-std=c++20",
    "-I$src",
    "-I$stbDir",
    "-I$sdlRoot\include",
    "-I$imguiDir",
    "-I$imguiDir\backends",
    "-I$x86Dir\include",
    "-I$x86Dir",
    "-I$build\mt32emu_gen",
    "-I$mt32CfgDir",
    "-I$mt32Src",
    "-I$jsonInc"
)
$appSources = @(
    "$src\main.cpp",
    "$src\dos\vga.cpp",
    "$src\dos\dos_machine.cpp",
    "$src\dos\x87_fpu.cpp",
    "$src\audio\audio_engine.cpp",
    "$src\input\head_tracker.cpp",
    "$src\input\hotas_mapper.cpp",
    "$src\net\net_udp.cpp",
    "$src\render\cockpit_3d.cpp",
    "$src\render\modern_renderer.cpp",
    "$src\sim\f16_flight_model.cpp",
    "$src\ui\overlay_ui.cpp",
    "$src\util\image_loader.cpp"
)
foreach ($f in $appSources) {
    $allObjs += Compile-Obj "c++" $f "app" $appFlags
}

Write-Host "Linking falcon30_native.exe..."
$outExe = Join-Path $build "falcon30_native.exe"
& $ZigExe c++ -target x86_64-windows-gnu -O2 `
    @allObjs `
    "$sdlRoot\lib\libSDL3.dll.a" `
    -lopengl32 -lwinmm -lws2_32 -ladvapi32 -lshell32 -lole32 -luuid `
    -o $outExe
if ($LASTEXITCODE -ne 0) { throw "Failed to link falcon30_native.exe" }

Copy-Item -Force "$sdlRoot\bin\SDL3.dll" (Join-Path $build "SDL3.dll")

Write-Host "Build succeeded!"
Get-Item $outExe, (Join-Path $build "SDL3.dll") | Format-Table Name, Length, LastWriteTime -AutoSize
