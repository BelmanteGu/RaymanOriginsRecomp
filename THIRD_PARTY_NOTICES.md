# Third-party notices

## XenonRecomp — MIT

Included as a git submodule (`tools/XenonRecomp`, fork at https://github.com/BelmanteGu/XenonRecomp, branch `rayman-port`).

Copyright (c) 2025 hedge-dev and contributors. See `tools/XenonRecomp/LICENSE.md`.

The fork also contains instruction implementations adapted from https://github.com/Nitch2024/XenonRecomp (a fork of XenonRecomp, MIT).

## Xenia — BSD-3-Clause

The Xenos half-float conversion helpers (`ppc_float_to_xenos_half`, `ppc_xenos_half_to_float` in the XenonRecomp fork) and the semantics of `vcmpbfp`, `vpkd3d128` and `vupkd3d128` are adapted from Xenia (https://github.com/xenia-canary/xenia-canary).

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
