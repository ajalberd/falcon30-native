param(
    [string]$Version = "0.3.0",
    [string]$ZigExe  = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$recomp   = Join-Path $repoRoot "recomp"
$src      = Join-Path $recomp "src"
$build    = Join-Path $recomp "build"
$tools    = Join-Path $recomp "tools"
$docs     = Join-Path $repoRoot "docs"
$dist     = Join-Path $repoRoot "dist"

# Locate Zig compiler for native Win32 GUI installer stub (+ optional main build fallback)
if ([string]::IsNullOrWhiteSpace($ZigExe)) {
    if ($env:ZIG_EXE -and (Test-Path $env:ZIG_EXE)) {
        $ZigExe = $env:ZIG_EXE
    } elseif (Get-Command zig -ErrorAction SilentlyContinue) {
        $ZigExe = (Get-Command zig).Source
    } elseif (Test-Path "C:\tools\zig\zig-x86_64-windows-0.14.1\zig.exe") {
        $ZigExe = "C:\tools\zig\zig-x86_64-windows-0.14.1\zig.exe"
    } else {
        throw "Zig compiler not found! Install Zig 0.14+ or pass -ZigExe <path>."
    }
}

# 1. Build falcon30_native.exe
& (Join-Path $recomp "build.ps1") -ZigExe $ZigExe

# Ensure Quickstart Keyboard Layout PNG exists
$keysPng = Join-Path $docs "Falcon30_Quickstart_Keys.png"
if (-not (Test-Path $keysPng)) {
    Write-Host "Generating docs\Falcon30_Quickstart_Keys.png..."
    python (Join-Path $tools "gen_quickstart_keys.py")
    if ($LASTEXITCODE -ne 0) { throw "Failed to generate Falcon30_Quickstart_Keys.png" }
}

# 2. Build Win32 GUI installer stub (with Common-Controls v6 + asInvoker manifest, GUI subsystem=2)
Write-Host "Building native Win32 GUI installer stub..."
$resFile = Join-Path $build "installer_res.res"
& $ZigExe rc /fo "$resFile" "$src\installer.rc"
if ($LASTEXITCODE -ne 0) { throw "Failed to compile installer.rc" }

$stubExe = Join-Path $build "installer_stub.exe"
& $ZigExe cc -target x86_64-windows-gnu -O2 "-Wl,--subsystem,windows" `
    "$src\installer_gui.c" "$resFile" `
    -luser32 -lkernel32 -lgdi32 -ladvapi32 -lshell32 -lole32 -luuid -lcomctl32 `
    -o $stubExe
if ($LASTEXITCODE -ne 0) { throw "Failed to build installer_stub.exe" }

# 3. Pack 1-shot single-file GUI installer
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$setupExe = Join-Path $dist "Falcon30-Native-Setup-v$Version.exe"

$packArgs = @(
    (Join-Path $tools "pack_installer.py"),
    $stubExe,
    $setupExe,
    "falcon30_native.exe=$build\falcon30_native.exe",
    "docs\Falcon30_Quickstart_Keys.png=$keysPng",
    "THIRD_PARTY_LICENSES.txt=$recomp\THIRD_PARTY_LICENSES.txt",
    "LICENSE_NATIVE.txt=$repoRoot\LICENSE",
    "README_NATIVE.md=$repoRoot\README.md"
)
$sdlDll = Join-Path $build "SDL3.dll"
if (Test-Path $sdlDll) {
    $packArgs += "SDL3.dll=$sdlDll"
}

& python @packArgs
if ($LASTEXITCODE -ne 0) { throw "Failed to pack installer!" }

# 4. Build Portable ZIP
$portDir = Join-Path $build "portable_stage"
if (Test-Path $portDir) { Remove-Item -Recurse -Force $portDir }
New-Item -ItemType Directory -Force -Path "$portDir\docs" | Out-Null

Copy-Item "$build\falcon30_native.exe"          "$portDir\falcon30_native.exe"
if (Test-Path $sdlDll) {
    Copy-Item $sdlDll                           "$portDir\SDL3.dll"
}
Copy-Item $keysPng                              "$portDir\docs\Falcon30_Quickstart_Keys.png"
Copy-Item "$recomp\THIRD_PARTY_LICENSES.txt"    "$portDir\THIRD_PARTY_LICENSES.txt"
Copy-Item "$repoRoot\LICENSE"                   "$portDir\LICENSE_NATIVE.txt"
Copy-Item "$repoRoot\README.md"                 "$portDir\README_NATIVE.md"
if (Test-Path "$repoRoot\launch_falcon3.bat.example") {
    Copy-Item "$repoRoot\launch_falcon3.bat.example" "$portDir\launch_falcon3.bat.example"
}

$portZip = Join-Path $dist "Falcon30-Native-v$Version-portable.zip"
if (Test-Path $portZip) { Remove-Item -Force $portZip }
Compress-Archive -Path "$portDir\*" -DestinationPath $portZip -CompressionLevel Optimal

# 5. Write SHA256SUMS.txt
$sumsPath = Join-Path $dist "SHA256SUMS.txt"
$h1 = (Get-FileHash $setupExe -Algorithm SHA256).Hash.ToLower()
$h2 = (Get-FileHash $portZip  -Algorithm SHA256).Hash.ToLower()
@(
    "$h1  Falcon30-Native-Setup-v$Version.exe"
    "$h2  Falcon30-Native-v$Version-portable.zip"
) | Set-Content -Encoding ASCII $sumsPath

Write-Host "`nRelease artifacts ready in $dist :"
Get-Item $setupExe, $portZip, $sumsPath | Format-Table Name, Length, LastWriteTime -AutoSize
Get-Content $sumsPath
