# build.ps1 — configure and build the native CLI (same toolchain as Codons:
# gcc + MinGW Makefiles). The browser build (clang --target=wasm32)
# arrives with milestone 0.6.
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
foreach ($dependency in @('cmake', 'gcc', 'mingw32-make')) {
    if (-not (Get-Command $dependency -ErrorAction SilentlyContinue)) {
        throw "Required tool is not on PATH: $dependency"
    }
}
$buildDirectory = Join-Path $PSScriptRoot 'build'
& cmake -S $PSScriptRoot -B $buildDirectory -G 'MinGW Makefiles' -DCMAKE_C_COMPILER=gcc "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
& cmake --build $buildDirectory --parallel
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
Write-Host "Ready: $(Join-Path $buildDirectory 'ribo.exe')"
