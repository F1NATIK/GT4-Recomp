# GT4 Recomp

**An unofficial native Windows port of Gran Turismo 4, made by static recompilation. No emulator.**

> ⚠️ **Work in progress.** Expect bugs, missing features and crashes.
>
> **You need your own copy of the game.** This project contains no game code, assets or disc images, and never will. You supply a 1:1 image of your own **Gran Turismo 4 (USA, SCUS-97328)** disc, and the game is built on your PC from it.

## What this is

GT4 Recomp converts the PlayStation 2 game code of Gran Turismo 4 into native C++ and compiles it into a Windows program. Graphics, sound and input that the PS2 hardware handled are re-implemented for the PC. The result runs directly on Windows, like a PC game, instead of through an emulator.

It's the sister project to the GT3 recomp and shares the same launcher.

## Features

- Native Windows build, running at a steady 60 fps
- Higher resolutions and widescreen shapes, or the original look with an optional scanline filter
- Launcher with display, graphics, controller/wheel, save and garage settings
- Optional race HUD (timing tower, radar, race map, start lights) and a telemetry page for a second screen or phone
- Game assets read straight from your own disc image

## Requirements

- Windows 10 or 11, 64-bit
- A graphics card with Vulkan 1.3 or OpenGL 4.5 support (any recent NVIDIA, AMD or Intel card)
- Your own **Gran Turismo 4 (USA) disc, SCUS-97328**, dumped to an ISO
- About 6 GB of free disk space to build (kept afterwards so updates rebuild in a minute), plus your ISO

## Getting started

1. **Dump your disc.** Make a full 1:1 ISO of your own GT4 disc, for example with ImgBurn's *Create image file from disc*. Downloaded copies are not supported.
2. **Download** the latest zip from the [Releases](../../releases) page and unzip it anywhere.
3. **Open the launcher** (`GT4Recompilation.exe`) and choose your ISO on the Play page.
4. **Click Build GT4.** The launcher turns the game code on your disc into a Windows program on your PC. Nothing is sent anywhere. The first build takes about 5 to 10 minutes. Your memory cards are never touched.
5. **Play.**

You don't need Visual Studio, Python or anything else installed. The release brings its own compiler ([llvm-mingw](https://github.com/mstorsjo/llvm-mingw)) and build tools (CMake and Ninja) in its `build-kit` folder.

## Building from source

The release's Build GT4 runs one command, `gt4setup build`, which you can also run yourself from a clone. The full steps are in [docs/BUILDING.md](docs/BUILDING.md). In short:

1. Clone this repo.
2. Get llvm-mingw, CMake and Ninja (or use Visual Studio 2022 or newer with **Desktop development with C++**, see BUILDING.md).
3. Build `gt4setup` and the recompiler (`ps2_recomp`) with them.
4. Run `gt4setup build` with your ISO. It copies the boot files off the disc, makes `CORE.elf`, translates the VU microprograms, runs the recompiler with the function list in `config/`, and compiles `GT4.exe`.

No Ghidra is needed: `config/` holds GT4's function addresses, so the recompiler makes exactly the same game code on every PC. These files are only addresses and PS2 kernel-call names, with no game code, and they only work with `CORE.elf` made from your own Gran Turismo 4 USA (SCUS-97328) disc (it is checked by SHA-256).

Generated game code (`core-generated/`), `CORE.elf` and the built `GT4.exe` are made from Sony's code. They stay on your PC and must never be committed or shared.

## Known issues

- Only the USA version (SCUS-97328) is supported.
- The launcher's pictures of GT4 cars and screens are grey placeholders in source builds.

## Reporting bugs

Open an [issue](../../issues) with your Windows version, graphics card, what you were doing, and the log file from the `logs` folder. Please don't attach game files, ISOs or saves.

## Credits

- Built on [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors (GPLv3).
- Third-party libraries are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Licence

The source code in this repository is licensed under the **GNU General Public License v3.0**. See [LICENSE](LICENSE).

## Disclaimer

This is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by Sony Interactive Entertainment or Polyphony Digital Inc. "Gran Turismo" and "PlayStation" are trademarks of their respective owners. No copyrighted game code or assets are included in this repository or its releases. You must own a legal copy of Gran Turismo 4 to use this software.
