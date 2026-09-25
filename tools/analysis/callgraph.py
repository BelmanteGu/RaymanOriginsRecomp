#!/usr/bin/env python3
"""Call graph of the ReXGlue output (rex/generated/default), for static analysis.

Usage: callgraph.py <generated dir> <out.json>

Each DEFINE_REX_FUNC(name) block becomes a node, and every `name(ctx, base)` call
inside it becomes an edge. Import thunks appear as `__imp__<Export>`. The output
is derived from the game, so keep it under private/.
"""
import glob
import json
import os
import re
import sys

DEF = re.compile(r'^DEFINE_REX_FUNC\((\w+)\)')
CALL = re.compile(r'\b(sub_[0-9A-F]{8}|__imp__\w+)\(ctx, base\)')


def build(gen_dir):
    calls = {}
    current = None
    for path in sorted(glob.glob(os.path.join(gen_dir, 'rayman_recomp.*.cpp'))):
        with open(path) as f:
            for line in f:
                m = DEF.match(line)
                if m:
                    current = m.group(1)
                    calls.setdefault(current, set())
                    continue
                if current:
                    for callee in CALL.findall(line):
                        if callee != current:
                            calls[current].add(callee)
    return {k: sorted(v) for k, v in calls.items()}


if __name__ == '__main__':
    graph = build(sys.argv[1])
    with open(sys.argv[2], 'w') as f:
        json.dump(graph, f)
    print(f'{len(graph)} functions, {sum(len(v) for v in graph.values())} call edges')
