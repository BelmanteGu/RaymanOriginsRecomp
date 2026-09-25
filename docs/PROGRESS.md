# Progress log

Technical notes on how Rayman Origins (Xbox 360) went from a raw `default.xex` to a native executable that boots and runs its render loop. The notes also cover what was hard, so that others recompiling Xbox 360 games can reuse the findings.

## Where things stand

**Update: the title screen renders, with audio, on macOS through ReXGlue.** See section 5; Windows and Android are in section 6. The rest of this section describes the milestone reached with our own runtime.

The recompiled game boots, initializes its engine, loads its UbiArt bundles, starts about 20 threads and submits frames to a GPU command processor at 1280×720 without crashing. Nothing is drawn yet: the GPU backend executes synchronization packets only.

| Milestone | Result |
|---|---|
| XenonRecomp translates the executable | 210 C++ files, 0 warnings, 0 `debugtrap` |
| Native build | 47 MB ARM64 executable (macOS, LLVM 23) |
| Boot | CRT, heap, TLS, threads, sync objects, XAM |
| Viability gate | Opens `secure_fat.gf`, localisation and every `.ipk` bundle |
| Render loop | 300 frames in 60 s through `VdSwap`, no crash |

## 1. Recompiling the executable

### Register save/restore helpers
Found by the byte patterns documented in XenonRecomp's README. Each pattern matches exactly once, and the distances between the eight helpers are identical to Sonic Unleashed's (same compiler runtime).

### Jump tables: 0 → 173
The stock XenonAnalyse finds **no** jump tables in this game. It matches exact contiguous instruction sequences calibrated on Sonic Unleashed, and this compiler reorders `lis`/`addi`/`rlwinm` and inserts `nop`s.

`tools/jumptables` resolves them differently:
- It finds the `cmplwi crN, rIdx, MAX` / `bgt crN, default` guard before each `bctr` (the scheduler can move the `cmplwi` up to 6 instructions away).
- It simulates the block with a small register evaluator (constants, scaled index, table loads, relative bases), so instruction order and interleaved unrelated instructions don't matter.
- It follows index aliases (`mr r31, r4` before `cmplwi r4`).
- **Unguarded tables**: 5 absolute tables have no guard (the index was validated earlier). Their size comes from the containing function: entries are counted while they point inside it. An entry pointing outside means a table of function pointers, and the table is rejected.

Caveat: XenonRecomp's "switch without table entry" warning only fires when the `bctr` is preceded by a `nop`, so zero warnings does **not** prove every table was found.

### Function boundaries
XenonRecomp trusts `.pdata`. Leaf functions without `.pdata` are split by static analysis, which fails in three cases, all handled by `tools/jumptables`:
1. **Jump tables** look like tail calls (14 functions).
2. **`mtctr` + `bdz` switches**: a counter-based dispatch (`mtctr r4; bdz a; bdz b; …`) that the analyzer ends at the first `blr` (49 functions).
3. **Functions reached only through pointers** (vtable methods, callbacks): with no `bl` and no `.pdata`, they are absorbed into the previous function and the indirect-call table has no entry for them (5816 functions). Candidates are code addresses found in data sections or built by `lis`/`addi` pairs, preceded by a terminator or padding, not conditional-branch targets, and not inside a `.pdata` function.

**Trap found by bisection**: a pointer-reached "function" placed on an **import thunk** replaced the import in the function table. The `NtCreateFile` thunk (`nop nop nop blr` before linking) became an empty function, and XAPI's file dispatch table called it instead of the kernel. The game could stat files but never open them. Image symbols (imports) are now treated as known function starts. `tools/bisect_ptr.py` automates this kind of search in a separate build directory.

### Instructions
52 opcodes were missing. 48 were ported from [Nitch2024/XenonRecomp](https://github.com/Nitch2024/XenonRecomp), the rest were written by hand. The ported code was x86/MSVC-only and had real bugs, fixed in [our fork](https://github.com/BelmanteGu/XenonRecomp/tree/rayman-port):

| Instruction | Problem |
|---|---|
| `vpkuhus128` | `_mm_packus_epi16` treats input as **signed**: `0x9000` saturated to 0 instead of 255 |
| `vpkswus128` | wrote the destination while still reading the source (broke when `vD == vA`) |
| `mulhd.`/`mulhdu.` | CR0 compared 32 bits of a 64-bit result; used MSVC `__mulh` |
| `lhbrx` | `rA == 0` used register r0 instead of the literal 0; only wrote 16 bits |
| `vcfpuxws128`, several VMX ops | x86-only intrinsics and a helper layer that only exists in the fork |

`vcmpbfp`, `vupkd3d128`/`vpkd3d128` (FLOAT16_2) follow Xenia, including the Xenos half-float format (no inf/NaN, exponent 31 is a normal number).

**Upstream XenonRecomp bug**: jump tables were emitted as `switch (r.u64)`. The guard (`cmplwi`) checks only the low 32 bits and the default is `__builtin_unreachable()`, so Clang dropped the bounds check and indexed the table with stale upper bits. Now `switch (r.u32)`.

**Timebase**: `mftb` becomes `__rdtsc()`, which on ARM reads a 24 MHz counter (Apple Silicon). The Xbox 360 timebase is 49.875 MHz, so the counter is scaled.

## 2. Building (macOS 14, Apple Silicon)

- Clang ≥ 18 is required. Homebrew builds LLVM from source on macOS 14, so `tools/setup.sh` downloads the official LLVM 23 release instead.
- `/usr/bin/ld` aborts when clang hands it LLVM 23's `libLTO.dylib`, and macOS then spends minutes in ReportCrash. We use **lld for every link**, including CMake's compiler identification step (so `-fuse-ld=lld` goes in the compile flags too).
- LLVM 23's libc++ headers need symbols that macOS 14's libc++ lacks, and linking LLVM's static libc++ puts two libc++ copies in one process. We compile against the **SDK's libc++ headers**.
- `regen_config.sh` only rewrites generated files whose content changed, so most rebuilds are incremental (~1–2 min instead of ~10).

## 3. Runtime

| Area | Notes |
|---|---|
| Memory | 4 GB guest space; `PageHeap` with reserve/commit semantics (Rayman builds its heap on `NtAllocateVirtualMemory`, unlike Sonic Unleashed); layout from Xenia; physical memory is 512 MB at `0xA0000000` so GPU physical addresses map one-to-one |
| Threads | pthreads with 16 MB host stacks (64 MB for the main thread); per-thread PCR/TLS/TEB; `ExCreateThread` calls the game's `XapiThreadStartup` |
| Sync | handle table + dispatcher (global lock + condition variable) with real timeouts, WaitAll/WaitAny; guest `KEVENT`/`KSEMAPHORE` bound by a signature in `WaitListHead`; critical sections and spinlocks with Clang atomics (no `std::atomic_ref` in macOS 14's libc++) |
| Files | `game:\`, `d:\`, `\Device\Cdrom0` → the user's dump (read-only); `cache:\` and saves → writable folders; case-insensitive lookups |
| XAM | local user, profile, content packages as folders, empty enumerators, automatic system UI answers, controller 1 connected at rest |
| Network | every call answers "network down" consistently |
| printf | Xenia's formatter, reading varargs from registers and the stack |
| Diagnostics | crash reporter with guest address, recompiled function and guest registers; checked indirect calls report the guest target that has no recompiled function |

## 4. GPU (first step)

A PM4 command processor thread consumes the ring buffer written by the game's Direct3D, executes synchronization packets and skips drawing ones. Three details were needed to get past Direct3D's `BlockOnFence` and GPU/CPU handshakes:

1. **Vblank on its own thread.** The command processor can block in `WAIT_REG_MEM` waiting for something the vblank handler writes.
2. **Scratch registers** (`SCRATCH_REG0..7`) are mirrored to `SCRATCH_ADDR + n*4` when enabled in `SCRATCH_UMSK`. Direct3D synchronizes through them.
3. **Command-stream interrupts are dispatched once per CPU in the mask, as that CPU.** The handler reads the current CPU from `PCR+0x10C` and acknowledges by clearing that CPU's bit.


## 5. ReXGlue: the title screen

[ReXGlue](https://github.com/rexglue/rexglue-sdk) (BSD-3-Clause) is an Xbox 360 recompilation SDK whose runtime is Xenia's kernel, Vulkan GPU and XMA audio, with an ahead-of-time codegen in the spirit of XenonRecomp. It ships macOS ARM64 builds (Vulkan through MoltenVK).

Its codegen needed exactly what `tools/jumptables` already knew about this game:

- The first run stopped on `Call to invalid or unregistered function at guest address 0x8297D9E8`, a function reached only through a pointer. Our list had it.
- ReXGlue rejects overlapping functions, which exposed a flaw in our sizing: a function containing a switch ran up to the next *structural* start and swallowed vtable methods behind it. Sizes now come from a single walk that knows every start (including pointer-reached ones), treats a branch to a known start as a tail call, and ignores pointer candidates between a switch and its cases. Result: 16 switch functions, 50 `bdz` functions, 5807 pointer-reached functions, 173 jump tables, zero conflicts.
- The GPU is a plugin (`--gpu_plugin=xenos`, `librexgpu-xenos.dylib` next to the executable) and the Vulkan loader must be pointed at MoltenVK's ICD (`VK_DRIVER_FILES`). `rex/run.sh` does both.

On an Apple M1 the game shows the Ubisoft logo, then the Rayman Origins title screen ("Press START"), with audio from the Mac speakers.

### Verifying without looking at the window

`screencapture` only records the desktop wallpaper when the terminal lacks the Screen Recording permission, so it can't prove anything renders. `RaymanApp::OnPostSetup` (`rex/src/rayman_app.h`) instead reads the guest's front buffer through `presenter()->CaptureGuestOutput()` every 10 seconds when `RAYMAN_CAPTURE=1` is set, and writes `captures/frame_NNN.ppm`. A 60-second run gives:

| Time | Frame |
|---|---|
| 10 s | Ubisoft logo |
| 20–30 s | black (movie/loading transition) |
| 40–60 s | title screen: jungle background, logo, "Press START", copyright line |

### Audio: our runtime vs ReXGlue

Both runtimes can dump what the game submits to the audio driver (`RAYMAN_AUDIO_DUMP=<file>`, stereo float32 at 48 kHz, 6-channel frames downmixed). In ReXGlue this is a mid-asm hook (`rex/rayman_hooks.toml`, `rex/src/hooks.cpp`) on the game's call to `XAudioSubmitRenderDriverFrame`; in `runtime/` it lives in the import itself.

| Runtime | Result over ~60 s |
|---|---|
| `runtime/` (ours) | sound in the first 10 s only (peak 0.41), then silence |
| ReXGlue | continuous sound for 59 s (peak ~0.4), with a 5 s gap at the screen transition |

Our driver pacing (one callback every 5.333 ms) is correct, so the game's mixer runs. The difference is the XMA decoder: music and most effects are XMA streams, and `runtime/` only allocates XMA contexts without decoding them. ReXGlue ships Xenia's decoder (FFmpeg), so reimplementing it in `runtime/` would duplicate working code. `runtime/` stays as a reference for the kernel semantics.

## 6. Other platforms

**Windows.** ReXGlue is Windows-first and ships a `win-amd64` SDK, so the same `rex/` project builds there with the `win-amd64-release` preset. See [WINDOWS.md](WINDOWS.md).

**Android (#17).** At least one Xbox 360 game already runs on Android through ReXGlue: [deivid22srk/redahm-android](https://github.com/deivid22srk/redahm-android) (NDK r27, Gradle, shared SDL3, APK built in CI). It has **no license**, so none of its code can be copied here; it only shows that the approach works. The plan:

1. Build the ReXGlue runtime for `android-arm64` from source. There is no prebuilt Android SDK, and this is the main risk.
2. Gradle project with the NDK and SDL3's Android activity. Android has native Vulkan, so no MoltenVK.
3. The APK contains no game data: the player copies their own dump to the app's folder.
4. On-screen controls; Bluetooth controllers work through SDL.

Expect a recent high-end phone (Snapdragon 8 Gen 1 class or better).

## 7. Toward a native renderer (static study)

ReXGlue draws by emulating the Xenos GPU (PM4 command stream, EDRAM, texture untiling in shaders), like Xenia. That works on an M1, but it is the heaviest part to carry to phone GPUs. Unleashed Recompiled took the other road: it replaces the Direct3D library linked into the executable with a translation layer over a modern RHI. This section maps what that road looks like for Rayman Origins. Everything here comes from static analysis. The game was not run.

Tools (the outputs are derived from the game and stay in `private/data/`):

- `tools/diag/imagedump`: writes the loaded XEX image as a flat file.
- `tools/analysis/callgraph.py`: call graph of the ReXGlue output (35,169 functions, 73,962 calls).
- `tools/analysis/xrefs.py`: `lis`/`addi` data references, mapped to their containing functions.

### What is linked

The XEX header's static library list: `D3D9`, `D3DX9`, `XGRAPHC`, `XAUDIO2`, `XMCORE` and `XAPILIB`, all from **XDK 2.0.20871**, built with MSVC 16.0.11886.

### No RTTI, but UbiArt names its classes

Only four RTTI type descriptors exist, all `std::` exceptions: the engine is built without RTTI. UbiArt keeps its own class-name strings instead (`GFXAdapter_Directx9`, `Adapter_Savegame_x360`, `SoundAdapter_*`, …), plus some source paths. Following the code that references those strings gives the same orientation RTTI would.

### The graphics layers

| Layer | Where | How it was found |
|---|---|---|
| UbiArt `GFXAdapter_Directx9` | code in `0x82120000`–`0x82140000`; vtable of **157 methods** at `0x820020A8` | references to the class-name string at `0x820007CC`; vtables scanned as runs of function pointers |
| Shader loading | `0x821316A8`, `0x821317B0` (`"Shaders/compiled/x360/%ls"`), registration at `0x8213A640` | string references |
| Frame present | engine `0x82139EE8` → `0x826D41B8` → `0x826D3A50` (the only caller of `VdSwap`) | callers of the kernel's `Vd*` exports, which only D3D calls |
| D3D9 core | around `0x826C0000`–`0x826F0000`: device init `0x826DCE48` (`VdInitializeEngines`), ring buffer `0x826DB368`, display mode `0x826DCC78` | same |
| D3DX9 | around `0x82600000`–`0x826C0000`, including the HLSL compiler (linked, never needed at runtime: every shader ships precompiled) | `D3DX:` and compiler strings |

The adapter calls **114 distinct D3D/D3DX functions**, which bounds the API surface a translation layer has to cover. Part of the Xbox 360 D3D API is inline (state setters that write straight into the device structure), so the first implementation task is to separate the out-of-line calls from inline state and decide which layer to hook: the 114 D3D functions (Unleashed's approach, whose GPL-3.0 layer we may reuse) or the adapter's 157 virtual methods.

### Shaders: 31 files, 18 KB

Every game shader is in `bootsequence_X360.ipk` under `shaders/compiled/x360/`:

| Family | Pixel | Vertex |
|---|---|---|
| `renderpct` (main 2D/2.5D renderer) | 8 | 14 |
| `afterfx` (post effects) | 2 | 2 |
| `font` | 2 | 1 |
| `movie` | 1 | 1 |

Each `.ckd` is a single, uncompressed XDK compiled-shader container (`0x102A1100` pixel, `0x102A1101` vertex), the input format of [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (MIT). Four more shaders are embedded in the executable's `.rdata` (`0x8207E0A8`–`0x8207E7D8`): D3D's own clear/resolve shaders. The whole set can be recompiled offline at build time, with no runtime shader translation.

IPK layout (version 3, as observed): header `magic 0x50EC12BA, version, ?, data base, file count`, then entries `{offset count, size, compressed size, timestamp u64, offsets u64[count], name length, UTF-16BE path}`. Data is at `data base + offset`.

### Unleashed's device layout fits

Unleashed Recompiled's video layer (`gpu/video.cpp`, GPL-3.0) hooks 42 D3D functions (`CreateDevice`, resource creation and locking, `SetTexture`, `SetRenderTarget`, `Clear`, `SetViewport`, the draw calls, shaders, vertex declarations, `Present`). It covers the inline part of the API by building the guest device itself:

- The XDK's inline `SetRenderState` calls through a table of function pointers inside the device (`lwz rX, 0x40 + 4*n(device)` → `mtctr` → `bctrl`). Unleashed fills that table with its own setters.
- Sampler states and shader constants are read from the device structure at draw time.

The device layout is XDK-version specific, so it was checked against Rayman Origins (XDK 20871). A scan of the executable for that inline pattern gives:

| Slot table | Unleashed's `GuestDevice` | Rayman Origins |
|---|---|---|
| Render-state setters | 0x65 entries at `+0x40` | **101 (0x65) distinct slots at `+0x40`**, 9,300 inline call sites |
| Sampler-state setters | 0x14 entries at `+0x1D4` | starts at `+0x1D4`, **27 slots** (up to `+0x23C`) |

The render-state table matches exactly. The sampler table is longer in this XDK, so the padding after it has to be re-measured before the rest of the structure (sampler states, shader constants, viewport) can be trusted. Next: pin down the remaining `GuestDevice` offsets and map the 42 hooked functions to their Rayman addresses. The Vd* callers and the resource vtables at `0x82085B08`–`0x82086C04` give the anchors.

### Why the Mac is the right test bed

The recompiled code already runs as **ARM64** on Apple Silicon (SIMDe for VMX), which is the Android CPU architecture. Boot, threads, file I/O, audio and the title screen are all validated on ARM64, so most of the Android risk is exercised daily. What the Mac does not exercise is texture compression: Apple Silicon Macs decode BC (DXT), most phone GPUs don't.

### Plan

The kernel, audio (XMA), input and file system stay on ReXGlue. Only the GPU changes. The app can hand ReXGlue its own `GraphicsSystem` (`config_.graphics`) instead of the Xenos plugin.

1. **Shadow mode.** Hook the D3D entry points and pass every call through to the original code, so the Xenos emulation keeps drawing, while mirroring them to a native renderer on [plume](https://github.com/renderbag/plume) (MIT: Metal, Vulkan, D3D12) in a second window. First milestone: `Present` clears to a color. Second: textured quads (the Ubisoft logo and the title screen are pure 2D). `RAYMAN_CAPTURE` frames from both paths are compared.
2. **Shaders.** Batch-run XenosRecomp over the 31 + 4 shaders, and count failures early.
3. **Textures.** A per-platform conversion layer from day one: untile, endian swap, then BC as-is (Mac, desktop) or BC → RGBA8/ASTC/ETC2 (Android). The Mac build can force the Android path to test it.
4. **Replace mode.** When shadow frames match, stop passing calls through and run without the Xenos plugin: native Metal on the Mac.
5. **Android.** Same renderer on plume's Vulkan backend, with the ReXGlue runtime built for `android-arm64` (section 6).

## Next steps

- Native renderer, step 1 of section 7 (shadow mode on the Mac).
- Play past the title screen and check levels, movies (#16) and saves.
- Validate the ported VMX instructions against Xenia's PPC tests (#4).
- Android (#17), as described in sections 6 and 7.
