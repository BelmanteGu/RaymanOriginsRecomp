# Direct3D map (Rayman Origins, XDK 2.0.20871)

The D3D functions and device fields the native renderer has to replace. They come from static analysis: PM4 packets emitted, callers in the UbiArt adapter (`GFXAdapter_Directx9`, vtable at `0x820020A8`), and the device fields each function writes. **Inferred** means the role comes from behaviour, not from a symbol, and still needs confirming at runtime.

## Functions

| Address | Role | Evidence |
|---|---|---|
| `sub_826D41B8` | **Present / Swap** | only path to `VdSwap` (through `sub_826D3A50`); adapter `vt[66]` |
| `sub_826DCE48` | device/engine init | calls `VdInitializeEngines`, `VdSetGraphicsInterruptCallback` |
| `sub_826DB368` | ring buffer setup | calls `VdInitializeRingBuffer` |
| `sub_826D7588` | **DrawIndexed**(device, primitive, baseVertex, startIndex, indexCount) | emits `DRAW_INDX`; adapter draw methods `vt[95–98]`, `vt[108]` |
| `sub_826D7170` | Draw (variant) | emits `DRAW_INDX`; `vt[98]`, `vt[107]` |
| `sub_826D6C68` | Draw with inline data (UP) | emits `DRAW_INDX`; `vt[37]` via `sub_826D7128` |
| `sub_826E9B28` | shader upload before a draw | emits `IM_LOAD`; called by every draw |
| `sub_826E9938` | shader constant upload | emits `LOAD_ALU_CONSTANT` |
| `sub_826CBBB8` | **SetPixelShader** | stores the shader at `device+0x330C` (confirmed at runtime) |
| `sub_826CBDC0` | **SetVertexShader** | stores the shader at `device+0x3310` (confirmed at runtime) |
| `sub_826CD850` | **CreateVertexShader**(function) | adds 872 bytes of vertex-fetch data; confirmed at runtime |
| `sub_826CD668` | **CreatePixelShader**(function) | confirmed at runtime |
| `sub_826C6E30` | **SetStreamSource** (inferred) | args (stream, buffer, offset, stride) |
| `sub_826C6FD8` | **SetIndices** (inferred) | stores at `device+0x320C`, which DrawIndexed reads as the index buffer |
| `sub_826CB810` | **Set\*ShaderConstantF** (inferred) | copies N float4 to `device + (start+120)*16` |
| `sub_826C62C0`, `sub_826C6468` | texture/sampler binding (inferred) | indexed by sampler, `vt[33]`, `vt[98]`, `vt[128]` |
| `sub_826C99C8`, `sub_826C9AE8` | texture LockRect / UnlockRect (inferred) | `vt[125]`, `vt[127]`, `vt[129]` |
| `sub_826D5328` | **Release** | 25 callers; `vt[63]` calls it, then clears the pointers |
| `sub_826C42E0`, `sub_826C4320` | scene begin/end around Present (inferred) | `vt[66]`, `vt[154]`, `vt[155]` |
| `sub_826C4A20`, `sub_826C4A50`, `sub_826C4AB8`, `sub_826C5148`, `sub_826C5188`, `sub_826C51B8`, … | **render-state setters** | write a device field and a dirty bit in `device+0x10`. The adapter calls several directly, not through the device's setter table |

Clear and resolve are internal (`DRAW_INDX_2` emitters `sub_826D4840`, `sub_826D4998`, `sub_826D5920`, `sub_826E0758`, `sub_826E7A00`). Their public entry points are still to be found.

## Device structure

| Offset | Content | Status |
|---|---|---|
| `+0x10` | dirty flags (64-bit words) | confirmed: every setter ORs bits here |
| `+0x40` | render-state setter table, 0x65 entries | confirmed: 101 slots used inline, 9,300 call sites |
| `+0x1D4` | sampler-state setter table, 27 entries | confirmed |
| `+0x2934`, `+0x2948`, … | packed render-state registers | written by the setters |
| `+0x2B08` | fence / busy counter | used by `Set*Shader`, `SetStreamSource` |
| `+0x480` | fetch constants (Xenos registers `0x4800`), 0x300 bytes: texture `t` at `+0x480+24t`, vertex fetch `v` at `+0x480+8v`; D3D binds stream `s` to `v = 95 − s` | from `SetStreamSource` |
| `+0x780` / `+0x1780` | vertex / pixel shader float constants (registers `0x4000` / `0x4400`) | from the draw flush |
| `+0x2780` | boolean constants (register `0x4900`) | from the draw flush |
| `+0x2880`, `+0x28CC`, `+0x2920`, `+0x2934`, `+0x2964`, `+0x29B8`, `+0x2A50` | register blocks `0x2000`, `0x2100`, `0x2180`, `0x2200`, `0x2280`, `0x2300`, `0x2380` | from the draw flush |
| `+0x320C` | current index buffer (object: `+0x18` data address, bit 31 of word 0 = 32-bit indices) | from DrawIndexed |
| `+0x330C` / `+0x3310` | current pixel / vertex shader | confirmed at runtime |
| `+0x3668`, `+0x366C` | command buffer write pointer / limit (inferred) | compared before writing packets |

Unleashed's `GuestDevice` (0x5E00 bytes) is a guide for the order of fields, not for their offsets: this XDK's structure is larger.

## Shaders

All 31 game shaders (`bootsequence_X360.ipk`, `shaders/compiled/x360/`) convert with XenosRecomp and compile to a 53 KB SPIR-V cache (docs/PROGRESS.md section 7).

## Runtime capture (title screen, Apple M1)

`rex/src/native_capture.cpp` (`RAYMAN_NATIVE_CAPTURE=1`) hooks shader creation and the three draw entry points:

- The game creates **34 shaders**. **31 match the precompiled SPIR-V cache exactly** (the 31 `.ckd` files). The other 3 (`9D7D9F03C1801F8D` VS, `D6F4DC8BD4BDE385` and `247180442A9D01BC` PS) are created from the executable itself and need adding to the cache.
- A title-screen frame has **~130 draws, all indexed triangle lists, with 6 shader combinations**, and **no draw uses a shader outside the known set**.
