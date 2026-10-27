# Building GT4 Recomp

This repository holds only hand-written code and GT4's function addresses (`config/`). Everything made from the game
(its converted code, its VU microprograms, `CORE.elf`, the disc files) is produced on your own PC from your own disc
and must never be committed or shared. `.gitignore` keeps those folders out of git.

Players don't need any of this: the release's launcher has a **Build GT4** button that runs the same steps with the
tools in its `build-kit` folder. This page is for building from a clone.

## What you need

- Windows 10 or 11, 64-bit
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) (the `ucrt-x86_64` zip), [CMake](https://cmake.org/download/)
  3.20 or newer and [Ninja](https://github.com/ninja-build/ninja/releases). The release uses llvm-mingw 20261006,
  CMake 4.4.4 and Ninja 1.13.2.
- An internet connection the first time (CMake fetches the libraries in [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md))
- A 1:1 ISO of your own **Gran Turismo 4 (USA, SCUS-97328)** disc
- About 6 GB of free space

No Python and no Ghidra: `gt4setup` replaces the Python scripts in `tools/`, and `config/` holds the function list
that Ghidra was used for once.

## Steps

Below, `LLVM` is the unzipped llvm-mingw folder. Put CMake's `bin` folder and Ninja on your `PATH`.

1. **Build gt4setup and the recompiler.**
   ```
   set LLVM_MINGW_ROOT=LLVM
   set PATH=LLVM\bin;%PATH%
   cmake -S recomp\gt4setup -B build\gt4setup -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=recomp\cmake\windows-llvm-mingw.cmake
   cmake --build build\gt4setup
   cmake -S recomp -B build\tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=recomp\cmake\windows-llvm-mingw.cmake ^
       -DPS2X_BUILD_RECOMP=ON -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF
   cmake --build build\tools
   ```
2. **Make a tools folder** for `gt4setup build`, with this layout (copies or links):
   ```
   tools\ps2_recomp.exe        (build\tools\ps2xRecomp\ps2_recomp.exe)
   tools\llvm-mingw\           (LLVM)
   tools\cmake\bin\cmake.exe   (your CMake)
   tools\ninja\ninja.exe       (your Ninja)
   ```
3. **Build GT4 from your disc.**
   ```
   build\gt4setup\gt4setup.exe build --iso "<your GT4 ISO>" --source recomp --config config --tools tools --work work --install game-folder
   ```
   It prints `@step 1 7 boot: ...` and so on, one line per step:
   1. copies the boot files off the disc (`work\game`, with a SHA-256 manifest);
   2. unpacks `CORE.GT4` into `CORE.elf` (checked by SHA-256, so only the USA disc works);
   3. translates the VU0 and VU1 microprograms to C++ (`work\vu`);
   4. runs `ps2_recomp` with `config\core-gt4.toml` and the function list (`work\core-generated`, about 27,000 functions);
   5. runs it again with `config\late-function-starts.txt` added and collects the pointer-only functions (`work\gt4_late_functions.cpp`);
   6. sets up the CMake build (`work\build`, kept, so a rebuild after an update takes a minute);
   7. compiles `GT4.exe`.

   With `--install`, `GT4.exe` and the disc's files in `game\` are copied into that folder at the end. Memory cards in
   its `game\` folder are left alone. `--from <step>` (boot, elf, vu, recomp, late, configure, compile) starts again at
   a step, and `--deps <folder>` builds offline from local copies of raylib, glslang, volk and the Vulkan headers.
4. **Set up the game folder.** Put `GT4.exe`, the launcher (`GT4RecompilationFull.exe`, rename it `GT4Recompilation.exe`
   if you like) and the `pages/` folder side by side with `game\`, start the launcher and pick your ISO.

## With Visual Studio instead

`tools\build.ps1` builds with Visual Studio's compiler (2022 or newer, **Desktop development with C++**). It needs the
game code that steps 1 to 3 above make: after one `gt4setup build`, the `work` folder has it. Then:
```
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Generated work\core-generated -VuDir work\vu -LateFunctions work\gt4_late_functions.cpp
```

## Notes

- `recomp/` is based on [PS2Recomp](https://github.com/ran-j/PS2Recomp). The GT3 parts of the runtime are kept
  for reference only. Building GT3 (`PS2X_GAME=GT3`) is not supported from this repository.
- The game code is the same on every PC: a fresh `ps2_recomp` run with `config/` writes the same files byte for byte,
  with llvm-mingw or Visual Studio.
- The launcher's built-in pictures of GT4 cars and screens are replaced here by plain grey stand-ins.
- DLSS (NVIDIA Streamline SDK) and FSR 3.1 / FSR 4 (AMD FidelityFX SDK) are optional. They need those SDKs' headers
  under `sdk/` and Visual Studio; without them the build simply has no DLSS or FSR 3/4.
