# Build on Windows

The build uses Zig **0.13.0** for the 32-bit Windows ASI plugin and Python 3 for menu, shader and ZIP generation. The scripts expect the following local layout relative to the repository root.

| Path | Contents |
| --- | --- |
| `.tools/ziglang/` | The extracted Zig distribution, including `zig.exe` and `lib/` |
| `Orig_game/` | A local 1.4 installation with original `FrontEnd/FrontB.lzc` and `fx/*.efx`. For packaging, also supply the matching Widescreen Fix distribution here. |
| `prototype/game/` | A separate working copy with `FrontEnd/`, `fx/`, `scripts/` and the x86 `d3dx9_43.dll` runtime. The build writes generated game resources here. |

Use the Widescreen Fix build identified in the README. The packager checks the hashes of its ASI, loader and supporting assets. The shader and menu builders also check their source assets.

From PowerShell in the repository root, run:

```powershell
.\build.ps1 -Package
```

The plugin is written to `build/NFSU.LocalCircuit.asi`. The complete installable package is `build/NFSU-SplitScreen.zip`. For a direct two-player Circuit check against the working copy, run `.\Test-Circuit.ps1`. To test the menu flow, run the working copy’s `Speed.exe` normally.
