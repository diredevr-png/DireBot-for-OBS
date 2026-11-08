#!/usr/bin/env python3
"""Checks the add-on's DLL only calls functions every supported OBS has.

Windows won't load a DLL at all when one function it links to is missing
from the DLL it comes from (OBS's own log only says "Module ... not
loaded"). OBS 33 took some functions out of obs.dll, which stopped
add-on 0.8.1 loading - so every build is checked against each OBS's real
obs.dll and obs-frontend-api.dll.

    tools/check-obs-imports.py direbot-for-obs.dll OBS-31.1.1/bin/64bit OBS-33.0.1/bin/64bit ...

Each folder is one OBS's bin\\64bit (from its Windows zip). Prints what's
missing where and exits 1 if anything is. Plain Python, no extra packages.
"""
import os
import struct
import sys

CHECKED = ("obs.dll", "obs-frontend-api.dll")


class PE:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError(f"{path}: not a Windows DLL")
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError(f"{path}: not a Windows DLL")
        sections = struct.unpack_from("<H", d, pe + 6)[0]
        opt_size = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", d, opt)[0]
        if magic != 0x20B:
            raise ValueError(f"{path}: not a 64-bit DLL")
        self.dirs = opt + 112          # data directories (PE32+)
        self.sections = []
        s = opt + opt_size
        for i in range(sections):
            _name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", d, s + 40 * i)
            self.sections.append((va, max(vsize, rsize), raw))

    def off(self, rva):
        for va, size, raw in self.sections:
            if va <= rva < va + size:
                return raw + rva - va
        raise ValueError(f"address {rva:#x} is in no section")

    def cstr(self, rva):
        o = self.off(rva)
        return self.data[o:self.data.index(b"\0", o)].decode("ascii", "replace")

    def directory(self, n):
        return struct.unpack_from("<II", self.data, self.dirs + 8 * n)

    def exports(self):
        rva, size = self.directory(0)
        if not rva:
            return set()
        o = self.off(rva)
        count = struct.unpack_from("<I", self.data, o + 24)[0]     # NumberOfNames
        names = struct.unpack_from("<I", self.data, o + 32)[0]     # AddressOfNames
        no = self.off(names)
        return {self.cstr(struct.unpack_from("<I", self.data, no + 4 * i)[0]) for i in range(count)}

    def imports(self):
        """{dll name (lower case): set of function names}"""
        out = {}
        for n, delay in ((1, False), (13, True)):
            rva, _size = self.directory(n)
            if not rva:
                continue
            o = self.off(rva)
            step = 32 if delay else 20
            while True:
                if delay:
                    fields = struct.unpack_from("<8I", self.data, o)
                    name_rva, thunks = fields[1], fields[4]
                else:
                    fields = struct.unpack_from("<5I", self.data, o)
                    name_rva, thunks = fields[3], fields[0] or fields[4]
                if not name_rva:
                    break
                names = out.setdefault(self.cstr(name_rva).lower(), set())
                t = self.off(thunks)
                while True:
                    entry = struct.unpack_from("<Q", self.data, t)[0]
                    if not entry:
                        break
                    if not entry >> 63:              # by name, not by number
                        names.add(self.cstr((entry & 0x7FFFFFFF) + 2))
                    t += 8
                o += step
        return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    wants = PE(argv[1]).imports()
    bad = False
    for folder in argv[2:]:
        for dll in CHECKED:
            need = wants.get(dll, set())
            if not need:
                continue
            path = os.path.join(folder, dll)
            have = PE(path).exports()
            missing = sorted(need - have)
            if missing:
                bad = True
                print(f"MISSING in {path} ({len(missing)}): {', '.join(missing)}")
            else:
                print(f"ok: {path} has all {len(need)} functions the add-on calls")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
