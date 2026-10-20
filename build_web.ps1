# build_web.ps1 — build the Ribosome core for the browser:
# freestanding wasm32, no libc, our own shim. Requires clang (LLVM).
$ErrorActionPreference = 'Stop'
$clang = 'clang'
$src = @(
    'src\parse.c', 'src\check.c', 'src\interp.c', 'src\fault.c',
    'src\emit_wasm.c', 'src\lift.c',
    'web\wasm\shim.c', 'web\wasm\api.c'
)
$args = @(
    '--target=wasm32-unknown-unknown',
    '-nostdlib',
    '-O2',
    '-Wall', '-Wextra', '-Werror',
    '-Iinclude', '-Isrc', '-Iweb\wasm\include',
    '-Wl,--no-entry',
    '-Wl,--export-all',
    '-Wl,--allow-undefined',
    '-o', 'web\ribo.wasm'
) + $src
# clang picks up a broken wasm-opt.exe from the Node dir on PATH; hide it.
# Do NOT touch the caller's PATH: spawn clang with a sanitized environment.
$savedPath = $env:PATH
try {
    $env:PATH = ($savedPath -split ';' | Where-Object { $_ -notlike '*nvm4w*' }) -join ';'
    & $clang @args
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    $env:PATH = $savedPath
}
$size = (Get-Item web\ribo.wasm).Length
Write-Host "ok: web\ribo.wasm ($size bytes)"
