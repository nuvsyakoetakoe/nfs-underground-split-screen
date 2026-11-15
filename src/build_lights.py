"""Build car and world materials with anisotropic filtering and dual headlights."""
from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
XOR = bytes(x ^ 0xaa for x in range(256))
SOURCES = {
    "car": "4055e8f0ffa65752ad4d9f9cf36afbdf67a07dd9f0c15b8ba31a57a1f726fea7",
    "world": "d1cdbd788d564c4cd408a060d0eda4e99245f0eb54b18a4fcee848d2966e6c98",
    "worldbone": "52c6081a679a071c49300fce118627bd4500ce4f54ea45592909500f38fb37cd",
    "worldreflect": "50c69d273ebd6a7dea23dfcb10c51ad467325af2bc5e2e15ad42d89f9d89e2bb",
    "worldheadlight": "1a6ae241940df17a017168c43e5c324aa5d74fd3f0122d4835b9f7082115b42b",
    "worldheadlightreflect": "00a281d7b9eb7ea936ba2d647f693328e6b92a1e7bcf6375eb2a3a6eff556d9d",
}

def add_second_light(source, name):
    source = source.replace("float4x4 HeadlightWVP", "float4x4 SecondHeadlightWVP;\nfloat4x4 HeadlightWVP")
    source = source.replace("struct VtoP\n{", "struct VtoP\n{\n    float4 second : TEXCOORD4;")
    source = source.replace("VtoP OUT;", "VtoP OUT;\n    OUT.second = mul(IN.position, SecondHeadlightWVP);")
    source = source.replace("float4 headlight_sample = tex2D(headlight_sampler, IN.t1);",
                            "float4 headlight_sample = tex2Dproj(headlight_sampler, IN.t1);")
    source = source.replace("headlight_sample*headlight_clip_sample*HeadlightColor",
                            "(headlight_sample*headlight_clip_sample + secondBeam)*HeadlightColor")
    source = source.replace("    float4 output =" if name == "worldheadlight" else "    float3 diffuse =",
                            """    float2 uv = IN.second.xy / max(IN.second.w, 0.001f);
    float4 secondBeam = (IN.second.w > 0.001f) * tex2D(headlight_sampler, uv) * tex2D(headlight_clip_sampler, float2(IN.second.w * .005 + .5, 0));
""" + ("    float4 output =" if name == "worldheadlight" else "    float3 diffuse ="))
    source = source.replace("tex2D(reflected_sampler, IN.t3.xy)", "tex2Dproj(reflected_sampler, IN.t3)")
    source = source.replace("tex1D(headlight_clip_sampler, IN.t2)", "tex2D(headlight_clip_sampler, IN.t2.xy)")
    source = source.replace("compile vs_1_1", "compile vs_2_0").replace("compile ps_1_1", "compile ps_2_0")
    return source


for name, digest in SOURCES.items():
    file = Path("fx", name + ".efx")
    encoded = (ROOT / "Orig_game" / file).read_bytes()
    if sha256(encoded).hexdigest() != digest:
        raise ValueError(f"Unsupported {file}")
    source = encoded.translate(XOR)[:-1].decode("ascii")
    if name.startswith("worldheadlight"):
        source = add_second_light(source, name)
    # Only textured surfaces viewed at an angle benefit from this sampler state.
    start = source.index("texture = <diffusemap>;")
    end = source.index("};", start)
    sampler = source[start:end].replace("MINFILTER = LINEAR;", "MINFILTER = ANISOTROPIC;\n\tMaxAnisotropy = 16;")
    source = source[:start] + sampler + source[end:]
    (ROOT / "prototype/game" / file).write_bytes((source.encode("ascii") + b"\xff").translate(XOR))
