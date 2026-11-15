[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$GameDir,
    [switch]$Package,
    [string]$RuntimeZip,
    [string]$Zig = 'zig'
)
$ErrorActionPreference = 'Stop'
$game = (Get-Item -LiteralPath $GameDir).FullName
$exe = Join-Path $game 'Speed.exe'
if ((Get-FileHash -LiteralPath $exe).Hash -ne 'c0fad450912952f53809a54690d235aa8b4e25b9f46650693c7cbee1b4c074b7') {
    throw 'Expected the tested Windows PC 1.4 build of Speed.exe.'
}
if ($RuntimeZip) {
    if (!$Package) { throw '-RuntimeZip is used with -Package.' }
    $RuntimeZip = (Get-Item -LiteralPath $RuntimeZip).FullName
}
$compiler = (Get-Command -Name $Zig -CommandType Application -ErrorAction Stop).Source
$python = (Get-Command python -CommandType Application -ErrorAction Stop).Source

Push-Location $PSScriptRoot
try {
    $output = Join-Path $PSScriptRoot 'build/mod'
    if ([IO.Path]::GetFullPath($output) -eq $game.TrimEnd('\')) { throw 'Choose a game folder outside the build output.' }
    foreach ($folder in 'scripts', 'FrontEnd', 'fx') {
        [IO.Directory]::CreateDirectory((Join-Path $output $folder)) | Out-Null
    }
    & $compiler cc -target x86-windows-gnu -O2 -Wall -shared src/circuit.c src/world_views.c src/local_input.c src/local_camera.c src/local_menu.c src/local_garage.c src/split_options.c -lcfgmgr32 -lcomctl32 -lxinput9_1_0 -o build/NFSU.LocalCircuit.asi
    if ($LASTEXITCODE) { throw 'ASI compilation failed' }
    & $python src/build_menu.py $game $output
    if ($LASTEXITCODE) { throw 'Front-end build failed' }
    & $python src/build_lights.py $game $output
    if ($LASTEXITCODE) { throw 'Road shader build failed' }
    Copy-Item -LiteralPath build/NFSU.LocalCircuit.asi -Destination (Join-Path $output 'scripts')
    Write-Output "Built mod files in $output"
    if ($Package) {
        $arguments = @('src/package_mod.py')
        if ($RuntimeZip) { $arguments += @('--runtime-zip', $RuntimeZip) }
        & $python @arguments
        if ($LASTEXITCODE) { throw 'Mod packaging failed' }
    }
} finally { Pop-Location }

