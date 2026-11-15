"""Package the split-screen runtime with the Widescreen Fix build it was tested with."""
from hashlib import sha256
from pathlib import Path
import re
from tempfile import NamedTemporaryFile
from zipfile import ZIP_DEFLATED, ZipFile


ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / "prototype/game"
WIDESCREEN = "scripts/NFSUnderground.WidescreenFix"
PINNED = {
    "dinput8.dll": "d5a059aa467a7a7127c8f6169f79fa63ff0f55986ee9eb2fd9a281bebf2aa2e6",
    WIDESCREEN + ".asi": "b5415cf48b13ba42cd9064dad6c37db79fb7874b2d0945f5e6ec35621b52e7ed",
    WIDESCREEN + ".dat": "148d2e4c7e193d11f7946d84730c2d5471663e62ffa3e65f69e1ea88868a1aa2",
    WIDESCREEN + ".tpk": "aa0fc803dff6d066b3e9879033d3e401e7f36f42503ee153e8dcf5b3b355d3af",
}
RUNTIME = (
    "scripts/NFSU.LocalCircuit.asi", "d3dx9_43.dll", "FrontEnd/FrontB.lzc",
    "fx/car.efx", "fx/world.efx", "fx/worldbone.efx", "fx/worldreflect.efx",
    "fx/worldheadlight.efx", "fx/worldheadlightreflect.efx",
)


def main():
    output = ROOT / "build/NFSU-SplitScreen.zip"
    with NamedTemporaryFile(dir=output.parent, delete=False) as blank:
        temp = Path(blank.name)
    try:
        with ZipFile(temp, "w", ZIP_DEFLATED) as archive:
            for name, digest in PINNED.items():
                data = (ROOT / "Orig_game" / name).read_bytes()
                if sha256(data).hexdigest() != digest:
                    raise ValueError(f"Unexpected Widescreen Fix component: {name}")
                archive.writestr(name, data)
            for name in RUNTIME:
                archive.write(GAME / name, name)
            # Base public defaults on the supplied fix, not the local test profile.
            ini = WIDESCREEN + ".ini"
            config = (ROOT / "Orig_game" / ini).read_bytes()
            for key, value in ((b"SkipIntro", b"0"), (b"SMAA", b"1")):
                setting = rb"^(" + key + rb"[ \t]*=[ \t]*)[01]\b"
                config, count = re.subn(setting, rb"\g<1>" + value, config, flags=re.M)
                if count != 1:
                    raise ValueError(f"Expected one {key.decode()} setting in {ini}")
            archive.writestr(ini, config)

            for width, height in ((1920, 1080), (2560, 1440), (3840, 2160)):
                # RES.cfg2 stores a signed int with bit 31 set and two 16-bit dimensions.
                packed = ((width << 16) | height) - (1 << 31)
                archive.writestr(f"ResolutionPresets/{width}x{height}/RES.cfg2",
                                 f"RES: {packed}\r\n".encode("ascii"))
            archive.write(ROOT / "src/resolution_presets.txt", "ResolutionPresets/README.txt")
        temp.replace(output)
    finally:
        temp.unlink(missing_ok=True)
    print(output)


if __name__ == "__main__":
    main()
