param(
    [Parameter(Mandatory)][string]$GameDir,
    [switch]$Solo,
    [switch]$Menu,
    [ValidateSet('Auto','KeyboardPad','TwoPads')][string]$Layout = 'Auto'
)
$ErrorActionPreference = 'Stop'
$game = (Get-Item -LiteralPath $GameDir).FullName
$exe = Join-Path $game 'Speed.exe'
if ((Get-FileHash -LiteralPath $exe).Hash -ne 'c0fad450912952f53809a54690d235aa8b4e25b9f46650693c7cbee1b4c074b7') {
    throw 'Unsupported Speed.exe'
}
if (!(Test-Path -LiteralPath (Join-Path $game 'scripts/NFSU.LocalCircuit.asi'))) { throw 'Install the mod in the specified game folder first.' }
$previous = $env:NFSU_SPLIT_TEST
$oldSolo = $env:NFSU_SOLO_CONTROL
$oldLayout = $env:NFSU_INPUT_LAYOUT
try {
    $env:NFSU_INPUT_LAYOUT = $Layout
    $env:NFSU_SOLO_CONTROL = if ($Solo) { '1' } else { '0' }
    $env:NFSU_SPLIT_TEST = if ($Menu) { '0' } else { '1' }
    Start-Process -FilePath $exe -WorkingDirectory $game -PassThru
} finally {
    $env:NFSU_INPUT_LAYOUT = $oldLayout
    $env:NFSU_SPLIT_TEST = $previous
    $env:NFSU_SOLO_CONTROL = $oldSolo
}
