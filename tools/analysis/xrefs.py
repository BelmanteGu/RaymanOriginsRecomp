#!/usr/bin/env python3
"""Data cross-references in the flat image written by tools/diag/imagedump.

Usage (as a module): xrefs.load(image_bin, callgraph_json) -> Xrefs

Finds addresses built by `lis rX, hi` followed (within the same basic block
window) by `addi rY, rX, lo`, `ori rY, rX, lo` or a load/store `d(rX)`, and maps
each referencing instruction to its containing function (function starts come
from the call graph written by callgraph.py).
"""
import bisect
import json
import re
import struct

BASE = 0x82000000
TEXT = (0x82120000, 0x82984694)
WINDOW = 12  # instructions a lis value is tracked for


class Xrefs:
    def __init__(self, image, starts):
        self.image = image
        self.starts = starts
        self.refs = {}  # target address -> [instruction addresses]
        self._scan()

    def _scan(self):
        d = self.image
        lis = {}  # reg -> (value, index)
        lo, hi = TEXT
        for i, off in enumerate(range(lo - BASE, hi - BASE, 4)):
            ins = struct.unpack_from('>I', d, off)[0]
            op = ins >> 26
            rd = (ins >> 21) & 31
            ra = (ins >> 16) & 31
            imm = ins & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            addr = off + BASE
            if op == 15 and ra == 0:  # lis
                lis[rd] = ((simm << 16) & 0xFFFFFFFF, i)
                continue
            if ins in (0x4E800020, 0x4E800420):  # blr / bctr: new block
                lis.clear()
                continue
            src = lis.get(ra)
            if src and i - src[1] <= WINDOW:
                target = None
                if op == 14:  # addi
                    target = (src[0] + simm) & 0xFFFFFFFF
                elif op == 24:  # ori
                    target = src[0] | imm
                elif op in (32, 34, 36, 38, 40, 42, 44, 48, 50, 52, 54):  # loads/stores
                    target = (src[0] + simm) & 0xFFFFFFFF
                if target is not None:
                    self.refs.setdefault(target, []).append(addr)
            if op in (14, 15, 24, 31, 21, 32, 34, 40, 42) and rd in lis and op != 15:
                # the register was overwritten by something else
                if not (op == 14 and ra == rd) and rd != ra:
                    lis.pop(rd, None)

    def func_of(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        return self.starts[i] if i >= 0 else None

    def strings(self, pattern, lo=0x82000400, hi=0x820E901C):
        """(address, text) of printable strings in [lo, hi) matching pattern."""
        seg = self.image[lo - BASE:hi - BASE]
        out = []
        for m in re.finditer(rb'[ -~]{4,}', seg):
            if re.search(pattern, m.group(0)):
                out.append((lo + m.start(), m.group(0).decode()))
        return out

    def users(self, target):
        return sorted({self.func_of(a) for a in self.refs.get(target, [])})


def load(image_bin, callgraph_json):
    image = open(image_bin, 'rb').read()
    graph = json.load(open(callgraph_json))
    starts = sorted(int(k[4:], 16) for k in graph if k.startswith('sub_'))
    return Xrefs(image, starts)
