# Building and playing on Windows

The ReXGlue runtime targets Windows first: Vulkan talks to the GPU driver directly (no MoltenVK) and the SDK ships an official `win-amd64` build. The steps below mirror the macOS build. They have not been tested on Windows by the maintainers yet; please open an issue with the output if a step fails.

## Requirements

| What | Notes |
|---|---|
| Windows 10/11, x64 | |
| GPU with Vulkan 1.2+ | any recent NVIDIA, AMD or Intel GPU, with an up-to-date driver |
| [Visual Studio 2022 Community](https://visualstudio.microsoft.com/vs/community/) | workload **Desktop development with C++**, plus the individual components **C++ Clang Compiler for Windows** and **MSBuild support for LLVM (clang-cl) toolset**. It also provides CMake, Ninja and the Windows SDK. |
| [Git](https://git-scm.com/download/win) | |
| Your own copy of the game | the Xbox 360 retail `default.xex` without title updates (SHA-256 `1444bbea…e9dfdc`) and its data files |
| ~15 GB free disk | SDK, generated C++ and build tree |

## Build

Open **x64 Native Tools Command Prompt for VS 2022** (Start menu) and check that `clang++ --version` works. If it doesn't, add `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin` to `PATH`.

```bat
git clone --recurse-submodules https://github.com/BelmanteGu/RaymanOriginsRecomp.git
cd RaymanOriginsRecomp

:: ReXGlue SDK (the zip contains a win-amd64\ folder)
curl -L -o rex.zip https://github.com/rexglue/rexglue-sdk/releases/download/v0.10.0/rexglue-sdk-0.10.0-win-amd64.zip
mkdir tools\rexglue
tar -xf rex.zip -C tools\rexglue

:: Copy the files of YOUR OWN copy of the game (default.xex, *.ipk, ...) to private\game
mkdir private\game

cd rex
..\tools\rexglue\win-amd64\bin\rexglue.exe codegen rayman_manifest.toml
cmake --preset win-amd64-release -Drexglue_DIR=%CD%\..\tools\rexglue\win-amd64\lib\cmake\rexglue
cmake --build out\build\win-amd64-release
```

- `rex/rayman_hints.toml` (function boundaries and jump tables) is already in the repository, so `tools/jumptables` doesn't need to be built on Windows.
- `codegen` writes about 150 MB of C++ to `rex\generated\default`. The build takes 15–30 minutes the first time.
- The build copies `rexruntime.dll`, `rexgpu-xenos.dll` and `TracyClient.dll` next to `rayman.exe` automatically.

## Play

```bat
rex\run.bat
```

Keyboard: WASD move, Space jump (A), L attack (X), Enter start. Xbox controllers work through XInput/SDL with no setup.

Diagnostics:

- `set RAYMAN_CAPTURE=1` saves the guest frame to `captures\frame_NNN.ppm` every 10 seconds.
- `set RAYMAN_AUDIO_DUMP=audio.raw` writes the game's audio as stereo float32 at 48 kHz. Import it in Audacity as raw data.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `clang` not found when configuring | use the x64 Native Tools prompt, or add VS's `VC\Tools\Llvm\x64\bin` to `PATH` |
| `Could not find a package configuration file provided by "rexglue"` | check the `-Drexglue_DIR` path; it must end in `lib\cmake\rexglue` |
| Black window, no GPU errors | update the GPU driver; check that `vulkaninfo` lists your GPU |
| `Call to invalid or unregistered function` | the executable is not the supported build, or `rayman_hints.toml` was regenerated from a different one |

## Legal

`private\game` and `rex\generated\default` are ignored by git. They contain the game and code derived from it: never commit, upload or share them.
