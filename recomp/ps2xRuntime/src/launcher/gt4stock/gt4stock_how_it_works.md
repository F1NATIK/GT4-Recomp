# How GT4 Recompilation works

## About

GT4 Recompilation runs Gran Turismo 4 as a native Windows program. It is a static recompilation: the game's program was translated once, ahead of time, from the PlayStation 2's MIPS processor code into x86-64 Windows code, instead of being emulated instruction by instruction while you play. Every car, track, song and movie still comes from your own GT4 disc image, untouched.

## Is it a full recompilation?

> **Yes, it's a full recompilation.** All 27,350 functions in GT4's main program are in GT4.exe as x86-64 Windows code: 27,295 were statically recompiled (translated ahead of time), and the other 55, standard Sony system routines for talking to the PS2's operating system and its second processor, are replaced by native code that does the same job. GT4's programs for the PS2's 3D and maths chips are translated too, and the PS2's sound, disc, controller and memory-card modules are replaced by native code. No emulator or interpreter runs the game's code. The one leftover is a slow step-by-step version of the 3D and maths chips. Only development builds keep it, to check the translations. No test run has ever needed it, and it will be removed from the final launch build. The game's data still comes from your disc image, which is normal for a recompilation.

## 1. What's inside a PS2

A PS2 isn't one computer but several chips, each with one job, passing messages to each other. The **Emotion Engine (EE)** runs the game itself: physics, AI, menus, races. Beside it, two small maths processors, **VU0 and VU1**, do quick sums and build the 3D scenes. The **Graphics Synthesizer (GS)** draws the picture. A second, smaller processor, the **IOP**, runs the **IRX modules**: drivers for the disc, sound, controllers, memory cards, USB and network. The **SIF link** joins the EE and the IOP.

```
+-----------------------------------------------+
| EMOTION ENGINE (EE): the main processor       |
| runs the game program, CORE.GT4               |
|   VU0: maths helper    VU1: builds 3D scenes  |
|   IPU: movie decoder                          |
+-----------------------------------------------+
     |                         ^
     | drawing commands        | SIF link:
     v                         v requests and answers
+---------------------------+ +-----------------------------+
| GRAPHICS SYNTHESIZER (GS) | | I/O PROCESSOR (IOP)         |
| paints the picture and    | | runs GT4's IRX modules: the |
| sends it to the TV        | | drivers for the parts below |
+---------------------------+ +-----------------------------+
                                 |       |       |       |
                               disc    SPU2    pads,   USB,
                               drive   sound   memory  network
                                       chip    cards
```

The game never touches the disc, the sound chip or the controllers directly, because on a PS2 those are wired to the IOP, not to the main processor. Instead it sends a request over the SIF link ("read these sectors", "play this voice", "which buttons are held?"), and an IRX module on the IOP does the work and answers.

## 2. Emulation versus recompilation

An emulator pretends to be a PS2 while you play. It reads the game's instructions one at a time, works out what each means and acts it out, all session long.

A recompilation does the translation once, ahead of time. GT4's program was translated, function by function, into C++ source code, and Microsoft's compiler built that into an ordinary Windows program, GT4.exe. When you play, the game's logic runs as real Windows code.

An illustration (simplified, not real output from GT4): "load a number, add one, store it back".

```
PS2 machine code (MIPS)      What the recompiler writes (C++)
lw    v0, 0x10(a0)           ctx->v0 = read32(ctx->a0 + 0x10);
addiu v0, v0, 1              ctx->v0 = ctx->v0 + 1;
sw    v0, 0x10(a0)           write32(ctx->a0 + 0x10, ctx->v0);
```

The real code is wordier, but every PS2 instruction becomes a line or two like this. GT4's 6 MB program became about 27,350 C++ files.

The translated game still expects a PS2 around it, so GT4.exe also contains stand-ins for the PS2's other parts.

## 3. The IRX modules

### Why they are replaced, not recompiled

- **They talk to hardware your PC doesn't have.** Translate a disc module faithfully and you would have to fake a whole PS2 DVD drive underneath it: emulation by the back door.
- **The game only cares about the answers.** When GT4 asks for 362,112 bytes in one go (the table of contents of its main data file), it doesn't matter how the bytes arrive, only that the right ones come back the way the game expects. A little C++ that reads your disc image does that faster.

So each module GT4 loads is replaced by a **native service**: new C++ code inside GT4.exe that holds the same conversation with the game as the original module.

### One disc read, start to finish

1. **Game code** sends a read request over the SIF link: start at this sector, read this many bytes, put them here.
2. **Our disc service**, the stand-in for Polyphony's PDICDVD module, receives exactly the message the real module would.
3. **Your disc image:** it reads those sectors straight from your ISO file. GT4 works out where its files are itself, so no file lookup is needed.
4. **Game code** gets the data and a "done" signal, exactly as on a PS2, and carries on.

### How each conversation was learned

GT4 uses modules from version 3.0 of Sony's PS2 toolkit, plus Polyphony Digital's own (names starting with PDI) and a few others. Each module's conversation was worked out from the module files on GT4's own disc: which requests it takes, their sizes, the answers, the version it reports. A native service must match that byte for byte, or the game hangs or misreads data. Examples:

- **Loader (LOADFILE):** GT4 first asks for its version and expects "3000"; give a wrong answer and it asks forever. It then loads 23 modules in four groups, and every load must be accepted. GT4 can also load one small module from inside its own program instead of from the disc.
- **Disc (PDICDVD):** every request GT4 sends it is a 64-byte message. Besides reads, it takes the disc's identity, answers a dual-layer question (where does the second layer start? For GT4's disc: sector 1,317,040; that layer holds the movies), and sends the game a status report every tenth of a second.
- **Streaming (PDISTR):** STRP loads sound banks, PBGM plays music and narration (9 commands, including fades), VOIC streams extra sound into the sound chip, and MPG1/MPG2 stream the movies and their soundtracks.
- **Sound (PDISPU2, LIBSD):** one message of nearly 1 KB each frame, which can set all 48 of the sound chip's voices at once.
- **Controllers (DBCMAN, SIO2D, DS2U_D):** our service sends GT4 a DualShock 2 report 60 times a second from your PC controller, and passes vibration back.
- **Memory cards (MCMAN, MCSERV):** report their versions and serve GT4's save files.

### What each module does

| Modules | What they do |
|---|---|
| LOADFILE, FILEIO | Load the other modules; file and device checks |
| PDICDVD, CDVDFSV, LIBPDI | Read the disc; Polyphony's base library |
| PDISTR, PDISPU2, LIBSD | Sound effects, music, narration and movie sound |
| DBCMAN, SIO2D, DS2U_D, SIO2MAN | Controllers: your PC controller appears to GT4 as a DualShock 2 |
| MCMAN, MCSERV | Memory cards |
| PDIUSB | USB keyboard and mouse |
| LGDEV | Logitech wheel: your PC wheel appears to GT4 as a Logitech Driving Force Pro, with force feedback |
| USBD, USTORAGE | USB stick: a folder on your PC, where GT4 saves its photos |
| DEV9, SMAP, INET, INETCTL, NETCNF, MSIFRPC, RTINETB, PDINETCNF | Network: GT4's LAN battle, on your local network only |
| EYETOY | Camera: in GT Mode, showing the GT shirt design to the camera at the Nike dealer gives you the NIKE One 2022. A webcam or a built-in shirt picture does the job |
| POWOFF | Power button: closing the window shuts GT4 down cleanly, like the PS2's power button |
| PADMAN, MTAPMAN | Used only by GT4's small boot loader. The recompilation starts at the main program, so the game never calls them |

## 4. What stands in for the PS2's chips

**3D and maths chips.** GT4 loads one program into each vector unit at start-up. Both were translated and checked against a slow step-by-step version over about 3 million runs in hidden test games (menus, car select and a race), with no differences. In a replay test the translated 3D program ran 12 to 14 times faster than imitating the chip.

**Graphics.** The renderer turns the GS's drawing commands into OpenGL. It is built to follow the GS's drawing steps in the same order and precision as a slow software reference GS, and during development frames are compared with that reference pixel by pixel. In the scenes tested so far, movies, menus and races match the reference exactly; small differences remain in the 3D car on the car select and in a few replay frames. A Vulkan renderer works in a separate test build, matched just as closely in the scenes tried, and is being added as an option; the Classic edition uses OpenGL. Mipmapping (softer textures for distant road and scenery, as on a PS2) has been written and is being checked: in the first race tests the picture matches the reference exactly, but the work is not reviewed or finished yet.

**Sound and movies.** The software SPU2 mixes all 48 voices; its reverb isn't modelled yet. The software IPU copies the real chip closely enough that the full intro movie plays with its music.

**Timing.** A PS2 gets a "vertical blank" (VBlank) signal 60 times a second, as the TV starts each picture. GT4.exe keeps that beat with a precise timer, and GT4 paces itself from it. In a race the physics moves one step per drawn frame, so a frame over 16.7 ms plays as a moment of slow motion. That is why the 60 fps goal is "no late frames", not a good average.

## 5. The parts of GT4.exe

- **Recompiler:** the open-source PS2Recomp tool, given a map of GT4's 27,350 functions. It writes the C++ in about 30 seconds; a full build of the 240 MB GT4.exe takes about 11 minutes on the development laptop.
- **Runtime:** the PS2 around the game: memory, threads, system calls, timers, interrupts and the channels that move data between chips.
- **Renderer and native services:** the stand-ins for the GS, the IRX modules, the sound chip and the movie decoder (sections 3 and 4).
- **Launcher:** GT4 Recompilation, Classic edition, a small program that starts GT4.exe with your settings. Pages: Play, Display (scanline filter), Controllers (pad, wheel, pedals, player 2, EyeToy webcam), Saves (cards, backups, photo folder, Save details), Cheats and About.

## 6. The tricky parts

The hard part is finding every piece of code: a function the recompiler never saw doesn't exist in GT4.exe, and the game stops the moment it calls one.

| Number | What it is |
|---|---|
| 24,616 | functions the Ghidra analysis tool found |
| 6,195 | extra entry points: jump tables, C++ class tables, error handlers |
| 383 | jump tables (switch statements) to follow |
| 27,350 | functions in the final recompile: 0 errors, 0 unhandled instructions |

- **Code hiding in data.** Five small security routines (SHA-1 and MD5 checksums) sit in GT4's data area, where a recompiler doesn't look. They were marked as code so they get translated.
- **Scrambled 3D code.** GT4's VU1 program is stored scrambled inside GT4's own program file and had to be decoded first.
- **A hidden maths bug.** The PS2's 128-bit multiply and divide instructions were translated wrongly at first. It broke the game's number reading: cropped menus, wrong race data. Fixed in the recompiler.
- **Graphics.** GT4 draws alternating half-height fields for an interlaced TV, reads its own images back for effects like wheel blur, and in New York's busiest sections the renderer makes up to about 28,000 drawing calls a second to the graphics card.

## 7. Your disc and your saves

Only the program was translated. Every car, track, sound, song and movie is read from your own GT4 disc image while you play: GT4.VOL (2.46 GB of game data) on the first layer, GT4L1.VOL (the movies) on the second. Nothing is extracted, converted or patched, and your image is never changed.

GT4 saves to a memory card: a folder of files, or a PCSX2-style .ps2 card file. The launcher can back cards up.

GT4 protects saves with several layers of scrambling: a random number per save, word swaps, a cipher built from two random-number generators, bit shuffles and two checksums. The garage file (credits and stored cars) is locked with a key kept in the main save, so the two only work as a pair. All of it is now understood, and a save can be decoded and rebuilt byte for byte. The launcher's Save details view shows a save's credits, days, cars, licences and progress, read only.

## 8. Words you'll see

| Word | Meaning |
|---|---|
| EE | Emotion Engine, the main processor. Runs the game. |
| VU0 / VU1 | Small maths processors beside the EE. VU1 builds the 3D scenes. |
| GS | Graphics Synthesizer, the drawing chip. |
| IOP | The second processor, which runs the IRX modules. |
| IRX | A driver module that runs on the IOP. |
| SIF link | The connection between the EE and the IOP. |
| Native service | Our C++ stand-in for an IRX module. |
| SPU2 / IPU | The sound chip / the movie decoder. |
| VBlank | The 60-times-a-second "new picture" signal. |
| CORE.GT4 | The disc file holding GT4's program. |
| Static recompilation | Translating a game's code once, ahead of time, before it ever runs. This is what GT4 Recompilation is. |
| Dynamic recompilation | Translating code in small pieces while the game is playing, as emulators like PCSX2 do. Not used here. |
| MIPS | The PS2 processor's instruction set: the code on the disc. |
| x86-64 | Your PC processor's instruction set: what GT4.exe is built in. GT4 Recompilation is a static recompilation from MIPS to x86-64 Windows. |

## 9. Notice and credits

GT4 Recompilation is an unofficial, fan-made project for personal use with your own copy of Gran Turismo 4. It is not affiliated with or endorsed by Sony Interactive Entertainment or Polyphony Digital. Gran Turismo 4 is a game by Polyphony Digital. "Gran Turismo" and "PlayStation" are trademarks or registered trademarks of Sony Interactive Entertainment Inc.
