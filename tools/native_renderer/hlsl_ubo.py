#!/usr/bin/env python3
"""Rewrites XenosRecomp HLSL so the SPIR-V build reads its constants from
uniform buffers instead of 64-bit buffer device addresses.

XenosRecomp's SPIR-V flavour passes the three constant blocks as uint64
addresses in push constants (vk::RawBufferLoad), which needs shaderInt64 and
bufferDeviceAddress. Stock Adreno drivers lack shaderInt64. Its D3D12 flavour
declares the same data as cbuffers with identical byte offsets
(VertexShaderConstants b0, PixelShaderConstants b1, SharedConstants b2, all in
space4), which DXC maps to descriptor set 4, bindings 0-2. This script makes the
SPIR-V build take that path, keeping the specialization constant.

Usage: hlsl_ubo.py in.hlsl out.hlsl
"""
import re
import sys

src = open(sys.argv[1]).read()

# 1) shader_common.h's first __spirv__ block (push constants, shared constant
#    loads, spec constant): replace it with cbuffer-style shared constants and
#    the spec constant.
common = re.compile(r'#ifdef __spirv__\s*\nstruct PushConstants.*?#endif\n', re.S)
replacement = '''[[vk::constant_id(0)]] const uint g_SpecConstants = 0;
#define g_SpecConstants() g_SpecConstants

#define DEFINE_SHARED_CONSTANTS() \\
    uint g_Booleans : packoffset(c16.x); \\
    uint g_SwappedTexcoords : packoffset(c16.y); \\
    float2 g_HalfPixelOffset : packoffset(c16.z); \\
    float g_AlphaThreshold : packoffset(c17.x);
'''
src, n = common.subn(replacement, src, count=1)
if n != 1:
    sys.exit('shader_common block not found in ' + sys.argv[1])

# 2) The per-shader constant block: '#ifdef __spirv__' followed by RawBufferLoad
#    defines -> take the cbuffer branch.
src, n = re.subn(r'#ifdef __spirv__(\s*\n(?:#define [^\n]*RawBufferLoad[^\n]*\n|\s*\n)+#else)',
                 r'#if 0\1', src)

# 3) DXC's SPIR-V backend requires cbuffer members in increasing packoffset
#    order (XenosRecomp emits them in usage order). Sort each cbuffer body;
#    macros and DEFINE_SHARED_CONSTANTS() (c16+) go after the members.
member = re.compile(r'^\s*\w+\s+\w+(\[\d+\])?\s*:\s*packoffset\(c(\d+)(?:\.([xyzw]))?\);\s*$')


def sort_body(match):
    head, body, tail = match.group(1), match.group(2), match.group(3)
    members, rest = [], []
    for line in body.split('\n'):
        m = member.match(line)
        if m:
            members.append((int(m.group(2)), 'xyzw'.index(m.group(3) or 'x'), line))
        elif line.strip():
            rest.append(line)
    members.sort(key=lambda t: (t[0], t[1]))
    shared = [l for l in rest if 'DEFINE_SHARED_CONSTANTS' in l]
    other = [l for l in rest if 'DEFINE_SHARED_CONSTANTS' not in l]
    lines = [m[2] for m in members] + shared + other
    return head + '\n' + '\n'.join(lines) + '\n' + tail


src = re.sub(r'(cbuffer \w+ : register\(b\d, space4\)\s*\n\{)(.*?)\n(\};)', sort_body, src, flags=re.S)

open(sys.argv[2], 'w').write(src)
