"""Lists the DLLs a 64-bit executable imports (without running it).

Use it to pick a proxy name for gamecache: a DLL the game imports that is not in
HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\KnownDLLs.

Usage: python -I pe_imports.py <game.exe> [function-filter,...]
"""
import struct
import sys

path = sys.argv[1]
filters = [f.lower() for f in sys.argv[2].split(",")] if len(sys.argv) > 2 else []
data = open(path, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsz = struct.unpack_from("<H", data, pe + 20)[0]
opt = pe + 24
if struct.unpack_from("<H", data, opt)[0] != 0x20B:
    sys.exit("only 64-bit executables are supported")
dirs = opt + 112
sections = []
for k in range(nsec):
    s = opt + optsz + k * 40
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, s + 8)
    sections.append((va, max(vsize, rsize), raw))


def off(rva):
    for va, size, raw in sections:
        if va <= rva < va + size:
            return rva - va + raw
    raise ValueError(hex(rva))


def cstr(o):
    return data[o:data.index(b"\0", o)].decode("ascii", "replace")


def walk(table_rva, desc_size, name_field, thunk_field):
    if not table_rva:
        return
    o = off(table_rva)
    while True:
        fields = struct.unpack_from("<" + "I" * (desc_size // 4), data, o)
        if not any(fields):
            break
        dll = cstr(off(fields[name_field]))
        funcs = []
        t = off(fields[thunk_field] or fields[0])
        while True:
            v = struct.unpack_from("<Q", data, t)[0]
            if not v:
                break
            funcs.append(f"#{v & 0xFFFF}" if v >> 63 else cstr(off(v & 0x7FFFFFFF) + 2))
            t += 8
        yield dll, funcs
        o += desc_size


imp_rva = struct.unpack_from("<I", data, dirs + 8)[0]
delay_rva = struct.unpack_from("<I", data, dirs + 13 * 8)[0]
for kind, gen in (("import", walk(imp_rva, 20, 3, 0)), ("delay", walk(delay_rva, 32, 1, 3))):
    for dll, funcs in gen:
        hits = [f for f in funcs if any(x in f.lower() for x in filters)] if filters else []
        print(f"{kind:6} {dll:40} {len(funcs):4} functions" + (f"  -> {', '.join(hits)}" if hits else ""))
