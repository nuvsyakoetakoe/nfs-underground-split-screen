# Troubleshooting

[← README](../README.md) · [Controls](CONTROLS.md) · [Русская инструкция](README.ru.md#решение-проблем)

## Split-Screen is missing from the menu

Check that you extracted **NFSU-SplitScreen.zip**, rather than a GitHub source archive, into the folder of the `Speed.exe` you actually launch. The ZIP's `scripts`, `FrontEnd` and `fx` folders must be copied with their contents, allowing the included files to be replaced. Avoid an extra `NFSU-SplitScreen` folder between the game executable and those folders.

Use the loader and Widescreen Fix included with this mod. Test on a separate clean game copy before combining it with an existing ASI loader or other mods.

## Input devices

1. Connect the gamepad before entering the race and confirm that it is available as an **XInput** controller in Windows.
2. Open **Options → Split Screen Options** and assign different devices to **P1 Device** and **P2 Device**.
3. Choose **Save Settings**, then start a new race. Input assignments are resolved when the race begins.
4. Keep the game window focused while driving.

For an initial check, use a keyboard for player one and an XInput gamepad for player two. See the [binding table](CONTROLS.md#driving). DirectInput-only controllers without conversion have not been verified.

## Settings do not persist

Choose **Save Settings** before leaving the options page. The game needs permission to write `SplitScreenOptions.ini` beside `Speed.exe`. Check that the file exists and changes after saving. If the game directory is protected, use a game copy in a folder your Windows account can write to.

## Resolution and widescreen settings

Choose a supported resolution under **Options → Display**. Additional Widescreen Fix options are in `scripts/NFSUnderground.WidescreenFix.ini`. Startup movies are enabled by default.

The release bundles the Widescreen Fix build it was tested with. Features or configuration examples for the latest upstream version may differ from this bundled build.

## Crashes or failure to launch

Compare your executable's SHA-256 with the [verified build](../README.md#compatibility). Reproduce the problem with the release ZIP on a separate clean PC 1.4 installation. Record whether it happens at startup, in the menu, while loading a race or during driving.

For a crash report, create a folder named `CrashDumps` beside `Speed.exe`, then reproduce the crash. The bundled Ultimate ASI Loader may write `.dmp` and `.log` files there. Attach the newest files if generated; if no dump appears, report the reproduction steps anyway.

## Report a problem

[Open a bug report](https://github.com/nuvsyakoetakoe/nfs-underground-split-screen/issues/new/choose) and include:

- Mod version and Windows version.
- SHA-256 of `Speed.exe` and whether a clean installation reproduces the problem.
- Race mode, track, cars and steps to reproduce; expected and actual results.
- Resolution, input devices, player assignments and other installed mods.
- A screenshot or short video, plus crash logs/dumps when available.

You can write the report in English or Russian.

## Uninstall

Restore the game folder backed up before installation. The mod replaces `FrontEnd/FrontB.lzc` and shader resources under `fx`, so deleting `scripts/NFSU.LocalCircuit.asi` alone is not a complete uninstall. If you have no backup, restore original resources from your own clean game installation. Keep a copy of your saves and custom settings before replacing the game folder.
