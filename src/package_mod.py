"""Package the split-screen runtime with the Widescreen Fix build it was tested with."""
from hashlib import sha256
from argparse import ArgumentParser
from io import BytesIO
from pathlib import Path
from tempfile import NamedTemporaryFile
from urllib.request import urlopen
from zipfile import ZIP_DEFLATED, ZipFile


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
RUNTIME_URL = (
    "https://github.com/nuvsyakoetakoe/nfs-underground-split-screen/"
    "releases/download/v1.0.1/NFSU1-SplitScreenMod.zip"
)
WIDESCREEN = "scripts/NFSUnderground.WidescreenFix"
PINNED = {
    "d3dx9_43.dll": "0b28546be22c71834501f7d7185ede5d79742457331c7ee09efc14490dd64f5f",
    "dinput8.dll": "d5a059aa467a7a7127c8f6169f79fa63ff0f55986ee9eb2fd9a281bebf2aa2e6",
    WIDESCREEN + ".asi": "b5415cf48b13ba42cd9064dad6c37db79fb7874b2d0945f5e6ec35621b52e7ed",
    WIDESCREEN + ".dat": "148d2e4c7e193d11f7946d84730c2d5471663e62ffa3e65f69e1ea88868a1aa2",
    WIDESCREEN + ".tpk": "aa0fc803dff6d066b3e9879033d3e401e7f36f42503ee153e8dcf5b3b355d3af",
    WIDESCREEN + ".ini": "c97a634b974df983fb02fd7dc2bd690420f676268ec20de9dc6b99d187e91e96",
}
GENERATED = (
    "scripts/NFSU.LocalCircuit.asi", "FrontEnd/FrontB.lzc",
    "fx/car.efx", "fx/world.efx", "fx/worldbone.efx", "fx/worldreflect.efx",
    "fx/worldheadlight.efx", "fx/worldheadlightreflect.efx",
)


def main(runtime_zip):
    if runtime_zip is None:
        print("Downloading runtime dependencies from the v1.0.1 release...", flush=True)
        with urlopen(RUNTIME_URL, timeout=60) as response:
            runtime_zip = BytesIO(response.read())
    output = BUILD / "NFSU1-SplitScreenMod.zip"
    with NamedTemporaryFile(dir=output.parent, delete=False) as blank:
        temp = Path(blank.name)
    try:
        with ZipFile(runtime_zip) as runtime, ZipFile(temp, "w", ZIP_DEFLATED) as archive:
            for name, digest in PINNED.items():
                data = runtime.read(name)
                if sha256(data).hexdigest() != digest:
                    raise ValueError(f"Unexpected runtime dependency: {name}")
                archive.writestr(name, data)
            for name in GENERATED:
                archive.write(BUILD / "mod" / name, name)
        temp.replace(output)
    finally:
        temp.unlink(missing_ok=True)
    print(output)


if __name__ == "__main__":
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-zip", type=Path, help="use a local v1.0.1 ZIP for offline builds")
    main(parser.parse_args().runtime_zip)
