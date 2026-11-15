param([switch]$Package)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $PWD '.tools/zig-cache'
    New-Item -ItemType Directory -Path build -Force | Out-Null
    & ./.tools/ziglang/zig.exe cc -target x86-windows-gnu -O2 -Wall -shared src/circuit.c src/world_views.c src/local_input.c src/local_camera.c src/local_menu.c src/local_garage.c src/split_options.c -lcfgmgr32 -lcomctl32 -lxinput9_1_0 -o build/NFSU.LocalCircuit.asi
    if ($LASTEXITCODE) { throw 'ASI compilation failed' }
    python src/build_menu.py
    if ($LASTEXITCODE) { throw 'Front-end build failed' }
    python src/build_lights.py
    if ($LASTEXITCODE) { throw 'Road shader build failed' }
    Copy-Item build/NFSU.LocalCircuit.asi prototype/game/scripts/
    if ($Package) {
        python src/package_mod.py
        if ($LASTEXITCODE) { throw 'Mod packaging failed' }
    }
} finally { Pop-Location }

