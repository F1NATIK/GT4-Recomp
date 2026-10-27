# Third-party notices

GT4 Recomp includes or builds on the following software. Each keeps its own licence; the full texts are in [`licenses/`](licenses/).

| Project | Used for | Licence | Text |
|---|---|---|---|
| [PS2Recomp](https://github.com/ran-j/PS2Recomp) | Recompiler and runtime base (`recomp/`) | GPLv3 | `PS2Recomp-GPL-3.0.txt` |
| [raylib](https://github.com/raysan5/raylib) 5.5 | Windows, input and OpenGL for the game and launcher | zlib | `raylib-zlib.txt` |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Vulkan renderer | Apache 2.0 or MIT | `Vulkan-Headers.txt`, `Apache-2.0.txt`, `Vulkan-Headers-MIT.txt` |
| [volk](https://github.com/zeux/volk) | Vulkan loader | MIT | `volk-MIT.txt` |
| [glslang](https://github.com/KhronosGroup/glslang) | Compiling the Vulkan shaders | BSD 3-clause and others | `glslang.txt` |
| [AMD FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (in `recomp/ps2xRuntime/third_party/fsr1`) | Upscaling filter | MIT | `AMD-FidelityFX-FSR1-MIT.txt` |
| [NVIDIA Image Scaling](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling) (in `recomp/ps2xRuntime/third_party/nis`) | Upscaling filter | MIT | `NVIDIA-Image-Scaling-MIT.txt` |
| [ELFIO](https://github.com/serge1/ELFIO) | Recompiler only | MIT | `ELFIO-MIT.txt` |
| [toml11](https://github.com/ToruNiina/toml11) | Recompiler only | MIT | `toml11-MIT.txt` |
| [{fmt}](https://github.com/fmtlib/fmt) | Recompiler only | MIT | `fmt.txt` |
| [rabbitizer](https://github.com/Decompollaborate/rabbitizer) | Recompiler only | MIT | `rabbitizer-MIT.txt` |
| [libdwarf](https://github.com/davea42/libdwarf-code) | Recompiler only | LGPL 2.1 | `libdwarf-COPYING.txt`, `libdwarf-LGPL-2.1.txt` |
| [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (Clang/LLVM, libc++, mingw-w64) | The compiler that builds GT4 on your PC; shipped in the release's `build-kit\tools\llvm-mingw` | Apache 2.0 with LLVM exception; mingw-w64's licences (ZPL, MIT and others) | `llvm-mingw-LICENSE.txt`, `mingw-w64-COPYING*.txt` |
| [CMake](https://github.com/Kitware/CMake) | Build setup; shipped in `build-kit\tools\cmake` | BSD 3-clause | `CMake-BSD-3-Clause.txt` |
| [Ninja](https://github.com/ninja-build/ninja) | Build runner; shipped in `build-kit\tools\ninja` | Apache 2.0 | `Apache-2.0.txt` |
| Microsoft Visual C++ (Visual Studio 2022 or newer) | Optional, building from source instead of llvm-mingw | Microsoft's licence; not included, you install it yourself | |

`gt4setup.exe` and `ps2_recomp.exe` in the release are built with llvm-mingw and include its C++ runtime (libc++, libunwind) and mingw-w64's runtime and winpthreads; their licences are in the files above. The release keeps each tool's own licence files in its folder too.

The libraries above, except FSR 1 and NVIDIA Image Scaling, are not stored in this repository: CMake downloads them when you build from a clone. The release brings their sources in `build-kit\deps` so Build GT4 works offline.

Optional parts that are not included and not built by default: Dear ImGui and rlImGui (debug UI only), FFmpeg (off by default), NVIDIA Streamline (DLSS) and AMD FidelityFX SDK (FSR 3.1 and FSR 4). If you add them, their own licences apply.
