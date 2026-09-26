# Third-party notices

## XenonRecomp — MIT

Included as a git submodule (`tools/XenonRecomp`, fork at https://github.com/BelmanteGu/XenonRecomp, branch `rayman-port`).

Copyright (c) 2025 hedge-dev and contributors. See `tools/XenonRecomp/LICENSE.md`.

The fork also contains instruction implementations adapted from https://github.com/Nitch2024/XenonRecomp (a fork of XenonRecomp, MIT).

## Xenia — BSD-3-Clause

Adapted from Xenia (https://github.com/xenia-canary/xenia-canary):

- The Xenos half-float conversion helpers (`ppc_float_to_xenos_half`, `ppc_xenos_half_to_float` in the XenonRecomp fork) and the semantics of `vcmpbfp`, `vpkd3d128` and `vupkd3d128`.
- The printf-style formatter in `runtime/kernel/guest_printf_core.inl` (from `xboxkrnl_strings.{h,cc}`).
- The semantics of the memory, synchronization and file system kernel imports.

```
Copyright (c) 2015, Ben Vanik.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the project nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## o1heap — MIT

Included as a git submodule (`thirdparty/o1heap`, https://github.com/pavel-kirienko/o1heap). Copyright (c) Pavel Kirienko. See `thirdparty/o1heap/LICENSE`.

## Unleashed Recompiled — GPL-3.0

The runtime layout (guest thread block, memory reservation) follows https://github.com/hedge-dev/UnleashedRecomp. Code ported from it keeps its GPL-3.0 license, which is also this project's license.

## SDL3 (Android Java glue) — zlib

`android/app/src/main/java/org/libsdl/app/` is copied from SDL3 (https://github.com/libsdl-org/SDL), Copyright (C) 1997-2026 Sam Lantinga. Licensed under the zlib license; the full text is in `android/app/src/main/java/org/libsdl/app/LICENSE.txt`.

## ReXGlue — BSD-3-Clause

`android/rexglue-patches/` contains patches against ReXGlue (https://github.com/rexglue/rexglue-sdk), which is BSD-3-Clause licensed and derived from Xenia. The patches are distributed under the same terms.

## Titan One, Barlow Semi Condensed — SIL Open Font License 1.1

The launcher's fonts in `android/app/src/main/res/font/`: Titan One, Copyright (c) 2011 Rodrigo Fuenzalida, with Reserved Font Name Titan; Barlow Semi Condensed, Copyright 2017 The Barlow Project Authors. Both from Google Fonts (https://github.com/google/fonts), licensed under the SIL Open Font License 1.1; the full texts are in `android/licenses/`.