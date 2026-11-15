"""Build the local multiplayer main menu and options selector."""
from hashlib import sha256
from pathlib import Path
from struct import pack, unpack_from, pack_into

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL = ROOT / "Orig_game/FrontEnd/FrontB.lzc"
OUTPUT = ROOT / "prototype/game/FrontEnd/FrontB.lzc"


def decompress(data):
    assert data[:6] == b"JDLZ\x02\x10"
    size = unpack_from("<I", data, 8)[0]
    out = bytearray()
    pos, flags, matches = 16, 1, 1
    while len(out) < size:
        if flags == 1:
            flags = data[pos] | 256
            pos += 1
        if matches == 1:
            matches = data[pos] | 256
            pos += 1
        if flags & 1:
            x, y = data[pos:pos+2]
            pos += 2
            if matches & 1:
                length, distance = ((x & 240) << 4) + y + 3, (x & 15) + 1
            else:
                length, distance = (x & 31) + 3, ((x & 224) << 3) + y + 17
            for _ in range(length):
                out.append(out[-distance])
            matches >>= 1
        else:
            out.append(data[pos])
            pos += 1
        flags >>= 1
    assert len(out) == size
    return bytes(out)


def literal_jdlz(data):
    # Literal-only JDLZ. The engine can load this using its ordinary resource loader.
    stream = b"\0" + b"".join(b"\0" + data[i:i+8] for i in range(0, len(data), 8))
    # PC decoder fetches the next flag before checking the compressed byte count.
    if len(data) % 8 == 0:
        stream += b"\0"
    return b"JDLZ\x02\x10\0\0" + pack("<II", len(data), 16 + len(stream)) + stream


def chunk(tag, data):
    return tag + pack("<I", len(data)) + data


def chunks(data):
    pos = 0
    while pos < len(data):
        tag, size = unpack_from("<4sI", data, pos)
        end = pos + 8 + size
        assert end <= len(data)
        yield tag, data[pos+8:end]
        pos = end


def fields(data):
    pos = 0
    while pos < len(data):
        name, size = unpack_from("<2sH", data, pos)
        end = pos + 4 + size
        assert end <= len(data)
        yield name, data[pos+4:end]
        pos = end


def field(name, value):
    return pack("<2sH", name, len(value)) + value


def string(value, encoding="ascii"):
    raw = (value + "\0").encode(encoding)
    return raw + bytes(-len(raw) % 4)


def name_hash(name):
    value = 0xffffffff
    for char in name.upper().encode("ascii"):
        value = (value * 33 + char) & 0xffffffff
    return value


def add_row(menu, options):
    source = "GameTrailers" if options else "QuickRace"
    new_name = "SplitScreenOptions" if options else "Multiplayer"
    caption = "SPLIT SCREEN OPTIONS" if options else "SPLIT SCREEN"
    highlight = "Highlight09" if options else "hightlight02"
    new_highlight = "Highlight10" if options else "hightlight07"
    source_id = 0x8c29 if options else 0x11c3b
    new_id, group_id = 0xf0001, 0xf0010
    group_tag = b"Obj\xcc"
    outer = list(chunks(menu[8:]))
    rows = list(chunks(dict(outer)[group_tag]))
    descriptors = {}
    for kind, obj in rows:
        if kind == b"FOb\xea":
            desc = dict(fields(next(chunks(obj))[1]))
            descriptors[unpack_from("<I", desc[b"OP"])[0]] = desc
    group = next(i for i, d in descriptors.items() if d[b"On"].rstrip(b"\0").decode() == highlight)
    copies = {source_id: new_id, group: group_id}
    for i, desc in descriptors.items():
        if b"PA" in desc and unpack_from("<I", desc[b"PA"])[0] in copies:
            copies[i] = group_id + len(copies)
    assert len(copies) >= 8 and not (set(copies.values()) & set(descriptors))
    positions = (["Audio", "Camera", "Car", "Controller", new_name, "Display", "EATrax",
                  "SavingOptions", "Credits", "GameTrailers"] if options else
                 ["UndergroundMode", "QuickRace", new_name, "STATISTICS", "Customize", "Options", "Online"])
    spacing = 30 if options else 35
    start = -142 if options else -141
    ys = {n: start + i*spacing for i, n in enumerate(positions)}
    highlights = {}
    for kind, obj in rows:
        if kind != b"FOb\xea":
            continue
        desc = dict(fields(next(chunks(obj))[1]))
        name = desc[b"On"].rstrip(b"\0").decode()
        if name in ys:
            events = [v for k, data in chunks(obj) if k == b"Scrp" for f, v in fields(data)
                      if f == b"EV" and v[:4] == pack("<I", 0xabc08912)]
            highlights[unpack_from("<I", events[0], 4)[0]] = ys[name] + 15
    highlights[group_id] = ys[new_name] + 15

    def transform(obj, clone):
        result = []
        desc = dict(fields(next(chunks(obj))[1]))
        ident = unpack_from("<I", desc[b"OP"])[0]
        name = desc[b"On"].rstrip(b"\0").decode()
        if clone:
            ident = copies[ident]
            if name == source:
                name = new_name
            elif name == highlight:
                name = new_highlight
        y = unpack_from("<f", desc[b"SA"], 32)[0]
        new_y = ys.get(name, highlights.get(ident, y))
        for tag, body in chunks(obj):
            if clone and tag == b"ObjD":
                parts = []
                for f, v in fields(body):
                    if f == b"OP":
                        v = pack("<II", ident, name_hash(name)) + v[8:]
                    elif f == b"PA":
                        v = pack("<I", copies[unpack_from("<I", v)[0]])
                    elif f == b"On":
                        v = string(name)
                    elif name == new_name:
                        if f == b"St":
                            v = string(caption, "utf-16le")
                        elif f in (b"SL", b"SH"):
                            if options:
                                v = string("") if f == b"SL" else bytes(4)
                            else:
                                v = string("MU_MAIN_MULTIPLAYER") if f == b"SL" else pack("<I", 0x93de931c)
                    parts.append(field(f, v))
                body = b"".join(parts)
            if clone and tag == b"Scrp" and name == new_name:
                body = body.replace(pack("<I", group), pack("<I", group_id))
            if new_y != y and tag in (b"Scrp", b"ObjD"):
                # Position tracks store an absolute base followed by relative
                # keys. Some stock highlight bases differ from the rest pose.
                parts = []
                target = None
                for f, v in fields(body):
                    if f == b"To":
                        target = unpack_from("<I", v)[0]
                    if f == b"SA" or (f == b"Kd" and target == 7):
                        offset = 32 if f == b"SA" else 8
                        v = bytearray(v)
                        base_y = unpack_from("<f", v, offset)[0]
                        pack_into("<f", v, offset, base_y + new_y - y)
                    parts.append(field(f, v))
                body = b"".join(parts)
            result.append(chunk(tag, body))
        return b"".join(result)

    output = []
    clone_group = []
    for kind, obj in rows:
        if kind == b"FOb\xea":
            ident = unpack_from("<I", dict(fields(next(chunks(obj))[1]))[b"OP"])[0]
            if ident in copies and ident != source_id:
                clone_group.append(chunk(kind, transform(obj, True)))
    for kind, obj in rows:
        if kind == b"Butn":
            obj = pack("<I", unpack_from("<I", obj)[0] + 1)
        if kind == b"FOb\xea":
            ident = unpack_from("<I", dict(fields(next(chunks(obj))[1]))[b"OP"])[0]
            if ident == group:
                output.extend(clone_group)
            if ident == source_id:
                output.append(chunk(kind, transform(obj, True)))
            obj = transform(obj, False)
        output.append(chunk(kind, obj))
    result = []
    for tag, data in outer:
        if tag == group_tag:
            data = b"".join(output)
        elif tag == b"Targ":
            new_fields = []
            for f, v in fields(data):
                if f == b"Mt":
                    ids = list(unpack_from("<" + "I"*(len(v)//4), v))
                    # Keep stock targets in order, with each clone next to its template.
                    # Each cloned subtree precedes its template in the object list.
                    ids = [ids[0]] + [target for i in ids[1:]
                                      for target in ((copies[i], i) if i in copies else (i,))]
                    v = pack("<" + "I"*len(ids), *ids)
                new_fields.append(field(f, v))
            data = b"".join(new_fields)
        result.append(chunk(tag, data))
    return chunk(menu[:4], b"".join(result))


def main():
    original = ORIGINAL.read_bytes()
    if sha256(original).hexdigest() != "76f368d5e5e30f96a8182bf457dba4491395e075c3f336d86a32012b56e16f90":
        raise SystemExit("Unsupported FrontB.lzc")
    targets = {b"MU_Main_PC.fng": False, b"MU_OptionsSelector_PC.fng": True}
    out = []
    for tag, data in chunks(decompress(original)):
        if tag == pack("<I", 0x30210) and data[4:8] == b"JDLZ":
            package = decompress(data[4:])
            name = package[40:104].split(b"\0")[0]
            if name in targets:
                rebuilt = add_row(package, targets.pop(name))
                packed = literal_jdlz(rebuilt)
                assert decompress(packed) == rebuilt
                block = chunk(tag, data[:4] + packed + bytes(-len(packed) % 4))
                out += [block, chunk(bytes(4), bytes((len(data) - len(block)) % 128))]
                continue
        out.append(chunk(tag, data))
    assert not targets
    temp = OUTPUT.with_suffix(".local.tmp")
    temp.write_bytes(literal_jdlz(b"".join(out)))
    temp.replace(OUTPUT)


if __name__ == "__main__":
    main()

