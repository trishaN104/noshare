# Builds the tests and benchmark locally with MSVC (no CMake required).
#
# Usage:
#   .\build.ps1          # build tests + bench (Release)
#   .\build.ps1 -Run     # build, then run tests and the benchmark
#
param([switch]$Run)

$ErrorActionPreference = "Stop"

# Auto-discover the C++ build environment (vcvars64.bat) instead of hardcoding a
# path. Prefer vswhere; fall back to searching the standard install roots.
function Find-VcVars {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $inst = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($inst) {
            $p = Join-Path $inst "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $p) { return $p }
        }
    }
    $roots = @("${env:ProgramFiles}\Microsoft Visual Studio",
               "${env:ProgramFiles(x86)}\Microsoft Visual Studio")
    $hit = Get-ChildItem -Path $roots -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue |
           Select-Object -First 1 -ExpandProperty FullName
    return $hit
}

$vcvars = Find-VcVars
if (-not $vcvars) {
    throw "Could not find the C++ build tools (vcvars64.bat). Install Visual Studio with the 'Desktop development with C++' workload, or use the CMake build instead."
}
$root = $PSScriptRoot
$out = Join-Path $root "build"
New-Item -ItemType Directory -Force -Path $out | Out-Null

# /wd4324 silences the *intentional* cache-line padding warning.
$flags = '/nologo /std:c++20 /EHsc /O2 /W4 /wd4324'
$cmd = "call `"$vcvars`" && cd /d `"$out`" && " +
       "cl $flags /I `"$root\include`" `"$root\tests\test_spsc.cpp`" /Fe:test_spsc.exe && " +
       "cl $flags /I `"$root\include`" `"$root\tests\test_mpsc.cpp`" /Fe:test_mpsc.exe && " +
       "cl $flags /I `"$root\include`" `"$root\tests\test_mpmc.cpp`" /Fe:test_mpmc.exe && " +
       "cl $flags /I `"$root\include`" `"$root\tests\test_shm.cpp`" /Fe:test_shm.exe && " +
       "cl $flags /I `"$root\include`" `"$root\tests\test_reference.cpp`" /Fe:test_reference.exe && " +
       "cl $flags /I `"$root\include`" `"$root\bench\bench_spsc.cpp`" /Fe:bench_spsc.exe && " +
       "cl $flags /I `"$root\include`" `"$root\bench\bench_compare.cpp`" /Fe:bench_compare.exe && " +
       "cl $flags /I `"$root\include`" `"$root\tools\latency_scope\scope.cpp`" /Fe:scope.exe && " +
       "echo BUILD_OK"

& $env:ComSpec /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

if ($Run) {
    Write-Host "`n--- running tests ---" -ForegroundColor Cyan
    & (Join-Path $out "test_spsc.exe")
    if ($LASTEXITCODE -ne 0) { throw "SPSC tests failed" }
    & (Join-Path $out "test_mpsc.exe")
    if ($LASTEXITCODE -ne 0) { throw "MPSC tests failed" }
    & (Join-Path $out "test_mpmc.exe")
    if ($LASTEXITCODE -ne 0) { throw "MPMC tests failed" }
    & (Join-Path $out "test_shm.exe")
    if ($LASTEXITCODE -ne 0) { throw "Shared-memory tests failed" }
    & (Join-Path $out "test_reference.exe")
    if ($LASTEXITCODE -ne 0) { throw "Reference validation failed" }
    Write-Host "`n--- running benchmark ---" -ForegroundColor Cyan
    & (Join-Path $out "bench_spsc.exe")
    & (Join-Path $out "bench_compare.exe")
    Write-Host "`n--- running latency microscope ---" -ForegroundColor Cyan
    & (Join-Path $out "scope.exe")
}
