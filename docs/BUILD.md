# Build on Windows

Install Zig **0.13.0** and Python 3. The commands below assume `zig` and `python` are on your PATH. Run them from the cloned repository, replacing the example game path with your own **clean Windows PC 1.4** installation.

```powershell
.\build.ps1 -GameDir "C:\Games\NFS Underground" -Package
```

`-GameDir` supplies the original game menu and shader files. The script reads these files and creates its working directories under `build\` in the repository. For a portable Zig installation, add `-Zig "C:\Tools\zig\zig.exe"`.

The finished package is **`build\NFSU1-SplitScreenMod.zip`**. Packaging downloads the project’s v1.0.1 release and verifies the SHA-256 of each bundled dependency (Widescreen Fix, its configuration and assets, ASI loader and DirectX runtime). The split-screen plugin, game menu and six shaders are built locally. To package offline, supply a previously downloaded release ZIP:

```powershell
.\build.ps1 -GameDir "C:\Games\NFS Underground" -Package -RuntimeZip "C:\Downloads\NFSU1-SplitScreenMod.zip"
```

Omit `-Package` to build just the plugin and modified game resources in `build\mod\`.

## Test a build

Extract your built ZIP beside `Speed.exe` in the game copy you want to play. Keeping a separate clean install allows you to rebuild again using the original game assets. Launch the modified copy normally for the menu flow or start a two-player Circuit test directly:

```powershell
.\Test-Circuit.ps1 -GameDir "C:\Games\Underground test"
```

Add `-Solo` for a single-player comparison or `-Menu` to launch through the menus.
