# Progress log

Technical notes on how Rayman Origins (Xbox 360) went from a raw `default.xex` to a native executable that boots and runs its render loop. The notes also cover what was hard, so that others recompiling Xbox 360 games can reuse the findings.

## Where things stand

**Update: the title screen renders.** See section 5.

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

On an Apple M1 the game shows the UbiArt logo, then the Rayman Origins title screen ("Press START"), with audio from the Mac speakers. `RAYMAN_CAPTURE=1` saves the guest frame every 10 seconds, to verify rendering without access to the window.

## Next steps

- Play past the title screen and check levels, movies (#16) and saves.
- Performance and correctness on MoltenVK.
- Validate the ported VMX instructions against Xenia's PPC tests (#4).
- Android (#17).
