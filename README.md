<p align="center">
  <img src="docs/logo.png" alt="NESSYSINTEGER" width="560">
</p>

# NESSYSINTEGER

**A cycle-accurate NES and Famicom emulator for Windows that treats the cartridge as a physical object.**

NESSYSINTEGER runs NES and Famicom games one clock at a time, the way the hardware does. Every signal between the console and the cartridge goes through a contact-by-contact model of the real cartridge connector. Tilt the cartridge, push the front-loader's tray down, blow on the contacts, lift individual pins or swap games with the power on, and the game reacts the way it would on a real console: flickering tiles, scrambled graphics, a blinking power light, a crash.

- **AccuracyCoin 144/144** at all four CPU/PPU clock alignments.
- **50 mapper numbers:** MMC1, MMC3, MMC5, Konami VRC1–VRC4, VRC6 and VRC7, Namco 163, Sunsoft FME-7/5B, Bandai FCG and more.
- **Expansion audio:** VRC6, VRC7 (FM), MMC5, Namco 163 and Sunsoft 5B.
- **Three consoles:** NES front-loader (NES-001), NES top-loader (NES-101) and Famicom, each with its own cartridge slot and reset wiring.
- **Realistic power-on state:** your emulated console has its own memory fingerprint.
- **Tools:** PPU viewer, debugger and memory editor, Game Genie and RAM-freeze cheats, save states, and a headless command-line runner.

Version **0.7.1**. The emulator runs on Windows. The command-line tools and test suites also build on Linux and macOS.

NESSYSINTEGER doesn't include any games. It loads `.nes` files in iNES and NES 2.0 format.

---

## Getting started

### Building

NESSYSINTEGER is plain C99 and uses only what ships with Windows, so there are no DLLs to carry around. Any of these toolchains works:

| Toolchain | How |
|---|---|
| **Scoop** | `scoop install gcc`, open a new terminal in this folder and run `.\build.bat` |
| **Visual Studio 2019/2022** or **Build Tools for Visual Studio** (*Desktop development with C++*) | Open the **x64 Native Tools Command Prompt for VS** in this folder and run `build.bat` |
| **w64devkit** (portable) | Run `w64devkit.exe`, `cd` into this folder and run `make` |
| **MSYS2** | `pacman -S mingw-w64-ucrt-x86_64-gcc make`, then run `make` in the UCRT64 shell |
| **CMake** | In Visual Studio, *File › Open › Folder…* picks up `CMakeLists.txt` |

That builds five programs:

| Program | What it is |
|---|---|
| `NESSYSINTEGER.exe` | The emulator |
| `nessys_cli.exe` | Headless runner for screenshots, audio capture, traces, scripted input and cartridge experiments |
| `accuracycoin_run.exe` | Runs the AccuracyCoin test ROM headlessly and prints every result |
| `mapper_test.exe` | Mapper and cartridge regression suite (773 checks) |
| `dma_trace.exe` | DMA and APU timing checks (28 checks) |

On Linux or macOS, `make` builds the four command-line programs (not the Windows front end), and `make test` runs the self-tests.

### First run

Open `tests/roms/selftest.nes` with *File › Open* or Ctrl+O, or drag it onto the window. It runs 18 hardware-behaviour checks and should finish on **ALL TESTS PASSED**. Then load a game, press **F9**, and start handling the cartridge.

### Controls

| | Player 1 | Player 2 |
|---|---|---|
| D-pad | Arrow keys | W A S D |
| A | X | H |
| B | Z | G |
| Start | Enter | Y |
| Select | Right Shift | T |

Xbox-style gamepads work too: pad 1 is player 1 and pad 2 is player 2. The pad's A and X buttons are the NES **B** button, and B and Y are **A**. Every key can be remapped in the `[Keys]` section of `nessysinteger.ini`, using Windows virtual-key codes (A–Z = 65–90, 0–9 = 48–57, Enter = 13, Space = 32, Left/Up/Right/Down = 37/38/39/40, Left/Right Shift = 160/161).

| Key | Action |
|---|---|
| **F1** | RESET button. Hold it to keep the console in reset |
| **F2** | Power switch |
| F3 / F4 | Pause / advance one frame |
| hold Tab | Fast forward |
| F5 / F7, 0–9 | Save state / load state, pick a slot |
| F6 | Debugger and memory editor |
| F8 | Screenshot |
| **F9** | Cartridge and connector panel |
| **F12** | PPU viewer |
| F11 or Alt+Enter | Fullscreen |
| Ctrl+O | Open a ROM |
| Ctrl+H | Hot-swap the cartridge with the power on |
| `.` | Pull the cartridge out / push it back in |
| `/` | Push the NES-001 tray down / release it |
| `[` `]` | Tilt the cartridge left / right |
| `;` `'` | Lean it toward the label side / the back |
| `-` `=` | Pull it out / push it in a little |
| Backspace | Reseat it straight |

Drag a `.nes` file onto the window to load it. Hold Ctrl while dropping to hot-swap it in.

---

## How it works

### One master clock

Everything runs from the NTSC master clock of 21.477 MHz. A CPU cycle is 12 master clocks and a PPU dot is 4, so the two chips keep their real 3:1 relationship, including the four phase alignments a real console can power up in (*Console › CPU/PPU clock alignment*). Nothing is batched per scanline or per frame: before every CPU bus access, the PPU is brought up to that exact master clock.

### CPU (Ricoh 2A03)

- Every CPU cycle is a real bus access, including the dummy reads and writes that software can observe.
- All 256 opcodes are implemented, the unofficial ones included. The JAM opcodes lock the CPU up.
- Within each 12-clock cycle, the bus access lands 5 clocks in and the NMI edge detector samples the /NMI line 9 clocks in. Those two values were calibrated on AccuracyCoin: they are the only ones with which every VBlank, NMI and rendering-toggle test passes at all four alignments.
- Interrupts are polled on the second-to-last cycle of each instruction. Cycles where DMA has halted the CPU don't advance the polling.
- The 2A03's internal data bus is modelled separately from the cartridge-side pins. Open-bus reads, `$4015` reads and DMA register conflicts behave as they do on hardware.
- SHA, SHX, SHY and SHS corrupt their target address the way the real chip does, including when DMA interferes.

### DMA

OAM DMA and DMC (sample) DMA follow the get/put cycle cadence documented on the NESdev Wiki:

- They halt only on read cycles, with alignment cycles.
- The "load" and "reload" DMC timings are modelled, as are the aborted-DMA and unexpected-DMA bugs and OAM/DMC overlap.
- A DMA that runs while the CPU is halted on `$4000-$401F` activates the 2A03's registers through its low address bits, the way the real chip does.

`dma_trace` replays the wiki's examples cycle by cycle.

### PPU (Ricoh 2C02)

- **Rendering:** dot by dot, 341 × 262 dots per frame, with the skipped dot on odd frames.
- **Memory fetches:** every fetch takes two dots on the real bus, an address-latch dot then a read dot. That matters to the mappers that watch the PPU bus: the MMC3's scanline counter, the MMC2/MMC4 tile latches and the MMC5's scanline detector all see the real fetch pattern.
- **`$2007` reads:** they go through the PPU's read state machine, which reads memory 4 dots after the CPU's read. That includes the glitches when the read lands in the middle of a fetch or happens while rendering.
- **Sprites:** sprite evaluation is the real OAMADDR state machine, with the sprite-overflow bug, misaligned OAM and the secondary-OAM address counter. Eight sprite output units with real X counters produce the pixels.
- **Reset:** after power-on or reset on an NES-001, the PPU ignores writes to PPUCTRL, PPUMASK, PPUSCROLL and PPUADDR until the first pre-render line, as the real one does.

### APU and sound

- **Channels:** two pulse channels, triangle, noise and DMC, with their real timers, length counters, envelopes, sweeps and frame counter, including the frame IRQ's timing quirks.
- **Mixing:** the channels go through the 2A03's non-linear mixing, then the NES's output filters (90 Hz and 440 Hz high-pass, 14 kHz low-pass). Output is 44.1 kHz.
- **Cartridge sound chips:** mixed in the way a real console mixes them (see [Boards (mappers)](#boards-mappers)).

### Colour

The palette isn't a fixed table of RGB values. It's generated by modelling the 2C02's composite video signal and decoding it the way an ideal NTSC TV would. The model is a square wave between two voltage levels, with 12 phases per colour cycle and emphasis attenuation. That covers all 64 colours with all 8 emphasis combinations.

### Power-on state

Real NES memory doesn't power up empty. By default (*Console › Power-on RAM contents › Like a real console*), your emulated console gets its own power-up fingerprint for CPU RAM, nametable RAM, palette RAM and OAM.

- Like real SRAM, the fingerprint is mostly the same at every power-on, with a few unstable bits that change each time.
- The palette's starting contents are based on blargg's dump from a real NTSC PPU.
- Cartridge work RAM and CHR-RAM get the cartridge's own pattern.
- *Swap in a different console* gives you a new fingerprint. Plain `$00`, `$FF`, random and striped patterns are also available.

Battery-backed save RAM is the exception: the cartridge's battery has kept it powered since the cartridge was made, so a save that has never been written starts out blank. CPU registers power up as documented (A = X = Y = 0, S = `$FD`, interrupts disabled).

### The cartridge connector

This is what sets NESSYSINTEGER apart. The console and the cartridge are joined by a model of the real connector, contact by contact:

| Console | Slot |
|---|---|
| NES front-loader (NES-001) | 72-pin connector with the push-down tray and the 10NES lockout chip |
| NES top-loader (NES-101) | 72-pin slot, no lockout chip |
| Famicom (HVC-001) | 60-pin slot, no lockout chip, with the cartridge audio loop |

Every CPU and PPU bus access, every mapper write, the IRQ line, the lockout chip's signals and the cartridge audio pass through the contact that carries them. Each contact's quality comes from four things:

- **How the cartridge sits:** how far it's inserted, whether the tray is down, and how it's tilted and leaning.
- **The console connector's pin springs:** brand new, typical used, worn out, a tight aftermarket replacement, or a refurbished original.
- **Oxidation and grime,** on the connector's pins and on the cartridge's own contacts.
- **Loose dust and breath moisture.**

From those, each contact is solid, intermittent or open. An intermittent contact is decided again on every single bus access, so the glitches look like the real thing. When a contact is open, the cartridge sees a floating input. What a floating input reads depends on its electrical behaviour, which you can choose under *Cartridge › Open pins read as*:

| Mode | An open input reads |
|---|---|
| **Realistic** (default) | The charge it last held, slowly leaking away, with coupling from the neighbouring contacts. A floating M2 picks up the 21 MHz clock next to it. |
| High | 1 (pull-up) |
| Low | 0 |
| Noise | Random bits |
| Last value | Whatever was last driven onto it |

The console's side of an open line is undriven. CPU reads see open bus, PPU reads see the low byte of the address left on the shared address/data lines, and /IRQ is held high by the console's pull-up.

#### What breaks, and why (NES 72-pin)

| Contacts | Signal | When it loses contact |
|---|---|---|
| 1, 72 / 36 | GND / +5 V | The cartridge loses power. The CPU reads open bus and runs garbage |
| 34, 35, 70, 71 | 10NES lockout | The lockout chip's reset loop: the picture and power LED blink once a second |
| 42–49 | CPU D7–D0 | Wrong opcodes reach the CPU, which crashes |
| 2–13, 39–41 | CPU A11–A0, A12–A14 | Code and data come from the wrong addresses |
| 14 | CPU R/W | Mapper writes get lost (floating high), or every read also hits the mapper (floating low) |
| 15 | /IRQ | Mapper interrupts stop, so split screens break |
| 50 | /ROMSEL | The ROM is never selected, or always selected and fights other chips on the bus |
| 23–29, 59–65 | PPU A0–A13 | Tiles are fetched from the wrong place |
| 30–33, 66–69 | PPU D0–D7 | Bit-planes stuck or flickering |
| 21, 56 | PPU /RD, /WR | CHR reads or CHR-RAM writes fail |
| 22, 57, 58 | CIRAM A10, CIRAM /CE, PPU /A13 | Nametable mirroring breaks |
| 54 | EXP 6 | Cartridge expansion audio drops out |

On the Famicom's 60-pin slot, the console's own sound leaves on contact 45, passes through the cartridge and comes back on contact 46. A plain cartridge just bridges the two, and a cartridge with a sound chip mixes its chip in. A dirty contact on 45 or 46 therefore silences the game completely.

#### The NES-001 tray

The front-loader's cartridge slides in at an angle and only meets the pins once the tray is pushed down and latched. The tray presses the cartridge into the connector's label-side row of springs, and every push-down bends them a little more. On a worn connector:

- games glitch or the power LED blinks until you press down hard on the cartridge;
- cleaning helps, but doesn't fix bent springs;
- bending the pins back, boiling the connector or replacing it does (these need the power off, because they mean opening the console);
- an aftermarket connector grips so tightly that the cartridge works without pushing the tray down at all.

Top-loading slots (Famicom, NES-101) wipe the contacts a little every time a cartridge goes in.

#### Blowing on it

Breath moisture conducts through the oxidation film, so blowing helps for a while. It also corrodes the cartridge's contacts a little every time. Isopropyl alcohol is the real fix (*Clean cart*), and the console side has its own cleaning kit (*Clean connector*).

#### The 10NES lockout chip (NES-001)

The lock chip in the console and the key chip in the cartridge exchange a bit stream continuously. If the exchange ever fails, the lock enters its die loop and pulses the reset line at 1 Hz, so the picture and the power LED blink. It keeps doing that until you press RESET to restart the handshake. The exchange fails on:

- an open or flaky CIC contact;
- a cartridge with another region's key;
- a cartridge with no key at all.

The cartridge's key can be set to *Licensed*, *Other region*, *No key* or *Unlicensed stun*. The stun is the voltage-spike trick some unlicensed cartridges used, and it usually, but not always, works. The lock chip can also be switched off, like the well-known pin-4 mod.

#### RESET and power

- **RESET (F1)** is a real button. While you hold it, the CPU stays in reset. On an NES-001 the reset line also holds the PPU, so the picture goes off. On a Famicom or NES-101 only the CPU and APU are held.
- **Power (F2)** is a real switch. When the power goes off, the 5 V rail takes tens of milliseconds to collapse. On the way down, the CPU misreads bits and runs off into the weeds, while battery-backed save RAM still accepts writes. Switching off without holding RESET therefore occasionally corrupts a byte of a save, which is why game manuals told you to hold RESET while turning the power off.

#### Hot-swapping

Ctrl+H, or dropping a ROM with Ctrl held, pulls the cartridge out and inserts another with the console still on. The running program finds a different ROM under it, just as it would if you swapped cartridges on a real console. While no cartridge is in, the CPU reads open bus.

#### Persistence

The connector's condition is saved per console model, and each cartridge's grime is saved per ROM, in `nessysinteger.ini`. Untick *Cartridge › Wear and dirt* for ideal contacts.

### Console models

*Console › Console model* chooses the console:

- **NES-001:** its reset line resets the PPU too. After power-on or reset, the PPU ignores register writes until the first pre-render line.
- **Famicom and NES-101:** the PPU's reset pin is tied high. At power-on the PPU starts about a frame before the CPU, so it's already warmed up when the game starts, and RESET resets only the CPU.
- **Auto** (the default): picks the Famicom for board families that were only ever made as Famicom cartridges, and the NES front-loader otherwise. Those families are VRC1–VRC7, Namco 163/175/340, Bandai FCG, Taito X1-005/X1-017 and NTDEC 2722.

Japanese games on shared boards, such as NROM, MMC1 or MMC3, can be switched to the Famicom by hand. The title bar shows the model in effect.

### Boards (mappers)

A mapper is the logic on the cartridge board. It switches which part of a large ROM the console sees, generates interrupts, adds RAM, and on some Famicom boards adds a sound chip. In NESSYSINTEGER each board is a small piece of hardware behind the connector. It sees only what reaches its contacts: the 15 CPU address lines, /ROMSEL, R/W and M2, plus the PPU address bus and its read and write strobes. A lifted address pin therefore affects a mapper's register decoding exactly as it would on a real board.

- **Bus watchers:** mappers that watch the PPU bus see the real dot-by-dot fetch pattern. That covers the MMC3's A12 scanline counter, the MMC2/MMC4 tile latches and the MMC5's scanline detector.
- **Cycle counters:** cycle-counting IRQs count M2 cycles. That covers the VRCs, FME-7, Namco 163, Jaleco, Irem, NTDEC 2722 and Kaiser boards.
- **Saves:** battery RAM and the Bandai 24C01/24C02 serial EEPROMs are saved to `.sav` files.
- **Variants:** NES 2.0 submappers select real board variants. Examples are the VRC2/VRC4 address-line wirings, VRC7a/b, MMC6, Bandai FCG versus LZ93D50, and the CNROM protection diodes.

**Expansion audio.** Famicom cartridges could mix their own sound chip into the console's audio. NESSYSINTEGER emulates five of them:

| Chip | Channels |
|---|---|
| VRC6 | 2 pulse + sawtooth |
| VRC7 | 6-channel FM: a cut-down Yamaha OPLL with the VRC7's own instrument ROM |
| MMC5 | 2 pulse + PCM |
| Namco 163 | Up to 8 time-multiplexed wavetable channels |
| Sunsoft 5B | 3 tone channels + noise + envelope |

On the Famicom the sound travels through the 45/46 audio loop. On an NES it uses the EXP 6 contact, as with the standard expansion-audio mod. *Console › Cartridge expansion audio* turns it off, which is how a stock NES without the mod sounds.

### Per-game files

These files are written next to the ROM:

| File | Contents |
|---|---|
| `game.sav` | Battery-backed RAM or EEPROM |
| `game.ss0` … `game.ss9` | Save states |
| `game.cht` | Cheats |
| `game-001.bmp` … | Screenshots |

Settings, key bindings, the console's fingerprint and the connector's condition live in `nessysinteger.ini` next to the program.

---

## The cartridge panel (F9)

The panel treats the cartridge and the connector as physical parts. It never takes keyboard focus, so you can handle the cartridge while you keep playing.

| Control | What it does |
|---|---|
| **Drag the cartridge** | Sideways tilts it; up and down pulls it out or pushes it in. Right-drag up and down leans it. |
| **Contact map** | Each contact shows its state: green is solid, yellow intermittent, red open, gold the power contacts, and a brown stain is oxidation. Hover a contact to see its signal, spring tension and oxidation. Click to force it open, flaky or closed; right-click puts it back to automatic. |
| **Pull out / insert** | Animated, including the tray. With the power on, it's a hot unplug. |
| **Tray down / release** | NES-001 only. |
| **Press down** (hold) | Pressure on the cartridge, which helps worn springs. |
| **Wiggle it** | A random nudge. |
| **Reseat straight** | Back to perfectly seated. |
| **All pins: auto** | Clears every contact you forced open, flaky or closed. |
| **Blow on it** | Contact now, corrosion later. |
| **Clean cart** | Isopropyl alcohol on the cartridge's contacts. |
| **Clean connector** | A cleaning kit through the console's connector. |
| **Bend pins back** | Restores worn springs, unevenly. Power off only. |
| **Boil the connector** | Restores the springs and loosens oxidation. Power off only. |
| **Replace connector…** | New, used, worn out, aftermarket or refurbished. Power off only. |

The panel also holds the open-input mode, the cartridge's CIC key, the lock-chip switch and the *Wear and dirt* switch, plus a status line with the console's power LED.

Things to try:

- Set *Open pins read as* to *High* and force contact **26 (PPU A3)** open while running the self-test: the text vanishes. With *Low*, it turns blue.
- Force contact **35** open on an NES-001 to start the lockout chip's blinking. Put it back, and the blinking keeps going until you press RESET.
- Choose *Replace connector… › Worn out*, then try a game with the tray up, the tray down, and while holding *Press down*.
- Hot-swap (Ctrl+H) between two games and watch the running program stumble into the new ROM.

## PPU viewer (F12)

*Tools › PPU viewer* shows the PPU's memory live. It reads without side effects, so it never disturbs the game.

- **Nametables:** all four, arranged by the cartridge's mirroring. The yellow outline is what each scanline actually showed, so a status-bar split appears as a step. Each row is drawn with the CHR banks of the scanline that displayed it, which keeps games that swap tile banks mid-screen, such as SMB3 and Kirby's Adventure, looking right. Tick *attribute grid* to see the 16×16 palette areas.
- **Pattern tables** `$0000` and `$1000`: 256 tiles each. Click a palette to recolour them. Hover a tile to see where it really lives: its CHR-ROM or CHR-RAM offset and 1 KB bank.
- **Palette RAM:** 4 background palettes and 4 sprite palettes. Colour 0 of every palette is the shared backdrop.
- **Sprites:** all 64 OAM entries. Hover one for its position, tile, palette and flips.

How the pieces fit together:

- **A tile** is 8×8 pixels stored in 16 bytes: two bit-planes, so 2 bits (4 colours) per pixel.
- **A nametable** is a 32×30 grid of tile numbers.
- **The attribute table** chooses one of the 4 palettes for each 16×16 area. That's why games build their worlds from 16×16 "metatiles".
- **Where tiles come from** depends on the board:
  - *Fixed CHR-ROM* (Super Mario Bros.): every area uses the same tiles, and only the palettes change between overworld, underground, castle and water.
  - *Bank-switched CHR-ROM* (SMB3 and Kirby's Adventure on the MMC3): animated tiles and per-level tile sets.
  - *CHR-RAM* (The Legend of Zelda, Lagrange Point): the CPU copies tiles in through `$2007`, which is also how fan translations add new fonts.

## Debugger, cheats and other tools

- **Debugger (F6):** CPU registers, PPU position, APU and mapper state, and a live disassembly. It steps by instruction, scanline or frame. Stepping mid-frame shows the half-drawn picture.
- **Memory editor (in the debugger):** edit the CPU bus, the PPU bus, OAM, cartridge RAM or internal RAM. Click a byte and type hex. Writes at `$8000` and up on the CPU bus, and to CHR on the PPU bus, patch the loaded ROM image.
- **Cheats (Tools menu):** Game Genie codes, 6 or 8 letters, and RAM freezes written as `0075:09`. They're saved per game.
- **Video:** window size 1×–6×, 8:7 pixel aspect like a TV, overscan crop and fullscreen.

## Command-line tools

### nessys_cli

A headless runner for test ROMs, screenshots and experiments.

```
nessys_cli rom.nes [options]
  -f N               run N frames (default 600)
  -s shot.ppm        save the last frame as a PPM image
  -w out.wav         record the audio (44.1 kHz, mono)
  -i SCRIPT          controller 1 input, e.g. "150-155:S;300-420:R"
                     (a frame or from-to range, then buttons: A B s=Select S=Start U D L R)
  -V view.ppm[:N]    save the PPU viewer picture (N = palette 0-7 for the tiles)
  -T trace.txt[:N]   log PPU and mapper writes and interrupts for the first N frames (default 150)
  -c auto|nes|nes101|fc        console model
  -p PIN=MODE        force a contact: off, on, flaky or auto (repeatable)
  -t S,R,P,D         cartridge pose: inserted, tilt, lean, dust (e.g. -t 1,0.3,0,0)
  -F real|high|low|noise|hold  what open inputs read as (default real)
  -a N               CPU/PPU clock alignment 0-3
  -m ADDR:LEN        dump CPU memory at the end (e.g. 0300:16)
  -g CODE            Game Genie code (repeatable)
  -q                 print only the result line
```

The exit code is 0 when a test ROM reports success through the `$6000` result protocol, which the self-test ROMs use. Some examples:

```
nessys_cli game.nes -p 26=off -F low -f 300 -s glitch.ppm    lift PPU A3, screenshot the result
nessys_cli game.nes -f 600 -i "150-155:S" -V view.ppm        press Start, save the PPU viewer picture
nessys_cli game.nes -f 200 -T trace.txt                      trace the first 150 frames
```

The trace collapses each run of `$2007` traffic into one line showing the count and VRAM address. It logs cartridge (mapper) writes and every NMI and IRQ entry, and stamps each line with frame, scanline and dot. It contains no ROM data, so it can be shared to diagnose a game.

### accuracycoin_run

Runs AccuracyCoin's "run every test" mode headlessly and prints every result.

```
accuracycoin_run AccuracyCoin.nes AccuracyCoin.asm [-a 0-3] [-r 0-4] [-v]
```

Get both files from the [AccuracyCoin repository](https://github.com/100thCoin/AccuracyCoin). The `.asm` file only supplies the test names and result addresses.

- `-a` picks the CPU/PPU clock alignment.
- `-r` picks the power-on RAM: 0 zeros, 1 `$FF`, 2 random, 3 stripes, 4 like a real console (the default).
- `-v` lists the passes too.

The exit code is 0 when every test passes.

### mapper_test and dma_trace

- **`mapper_test`** (773 checks) builds synthetic cartridges in which every PRG and CHR bank carries its own number, then drives each board through the connector model. It checks:
  - every bank window and nametable source;
  - the exact cycle each IRQ fires;
  - EEPROM read-back;
  - each sound channel's pitch, measured by autocorrelation;
  - all twelve VRC2/VRC4 wirings, and the MMC5's scanline IRQ and extended attributes;
  - the console models, contact physics, the lockout chip, holding RESET, and save corruption at power-off.
- **`dma_trace`** (28 checks) replays the NESdev Wiki's DMA examples cycle by cycle, plus AccuracyCoin's documented DMC, register-conflict, frame-IRQ and controller-strobe rules.

`make test` builds everything and runs the self-test ROMs, `dma_trace` and `mapper_test`.

---

## Accuracy and testing

| Test | Result |
|---|---|
| [AccuracyCoin](https://github.com/100thCoin/AccuracyCoin) | **144 / 144** at all four CPU/PPU clock alignments and with every power-on RAM pattern |
| Self-test ROMs (`tests/roms`) | All 18 hardware checks pass at all four alignments; the MMC1 and MMC3 tests pass |
| `mapper_test` | 773 checks, 0 failures |
| `dma_trace` | 28 checks, 0 mismatches |

During development, the CPU was also fuzzed against an independent 6502 model, matching registers, flags, memory and cycle counts on every opcode.

### Games tested so far

| Game | Board | Checked |
|---|---|---|
| Super Mario Bros. | NROM (0) | Title screen and gameplay |
| Super Mario Bros. 3 | MMC3 (4) | Title screen, World 1 map and status bar |
| Kirby's Adventure | MMC3 (4) | Title screen, level intro and gameplay |
| The Legend of Zelda | MMC1 (1) | Title screen, file select and name entry |
| Lagrange Point (English translation by Aeon Genesis) | VRC7 (85) | Title screen, name entry and text; FM music |
| Super Mario Bros. 2 (Japan), NTDEC cartridge conversion | NTDEC 2722 (40) | Title screen and gameplay. This conversion's title screen has no logo; that's in the ROM itself |
| Super Mario Bros. 2 (Japan), RetroZone release | Kaiser KS7032 (142) | Title screen and gameplay |

### Known issues

- MMC1 boards with 16 KB of save RAM (SOROM, used by games such as Nobunaga's Ambition) select the wrong save-RAM bank.
- Every MMC3 behaves like the MMC3B/C. The MMC3A's older IRQ behaviour (NES 2.0 submapper 4) isn't modelled yet.
- The Famicom Disk System (mapper 20) isn't emulated yet.
- PAL and Dendy timing, the Zapper and other peripherals aren't emulated.
- The FM, PSG and wavetable sound chips follow their documented behaviour, but haven't been compared with recordings of real hardware.
- Dragging or resizing the window pauses emulation (normal Win32 behaviour).

---

## Supported mappers

| Mapper | Board | Notes |
|---|---|---|
| 0 | NROM | |
| 1 | MMC1 (SxROM) | Includes SUROM/SXROM banking |
| 2, 94, 180 | UxROM, UN1ROM, UNROM (AND) | |
| 3, 185 | CNROM, CNROM with protection | 185 models the CHR-enable diodes (NES 2.0 submappers 4–7) |
| 4, 118, 119 | MMC3 / MMC6, TxSROM, TQROM | MMC6 (submapper 1): 1 KB RAM with per-half enables. 118: CHR A17 drives CIRAM A10. 119: CHR-RAM/ROM mix |
| 5 | MMC5 (ExROM) | All PRG/CHR modes, up to 64 KB PRG-RAM, ExRAM modes, extended attributes, fill mode, vertical split, scanline IRQ, multiplier, 2 pulse + PCM audio |
| 7 | AxROM | |
| 9, 10 | MMC2, MMC4 | |
| 11 | Color Dreams | |
| 13 | CPROM | |
| 16, 159 | Bandai FCG-1/2, LZ93D50 | Submapper 4 = FCG, 5 = LZ93D50. 24C02 / 24C01 serial EEPROM, saved like battery RAM |
| 18 | Jaleco SS8806 | 4/8/12/16-bit IRQ counter |
| 19 | Namco 163 | CIRAM / CHR-ROM nametables, 15-bit IRQ, up to 8 wavetable channels |
| 21, 22, 23, 25 | VRC2 / VRC4 | NES 2.0 submappers select the address-line wiring (VRC4a–f, VRC2a–c). Without one, both candidates are decoded |
| 24, 26 | VRC6a, VRC6b | 2 pulse + sawtooth |
| 32 | Irem G-101 | Submapper 1 = Major League |
| 33 | Taito TC0190 | |
| 34 | BNROM / NINA-001 | |
| 40 | NTDEC 2722 | SMB2J cartridge conversion. CD4020 IRQ: fires 4096 cycles after enabling, clears itself after 8192 |
| 64 | Tengen RAMBO-1 | Scanline or CPU/4 IRQ |
| 65 | Irem H-3001 | |
| 66, 140 | GxROM, Jaleco J11 | |
| 67, 68, 69 | Sunsoft-3, Sunsoft-4, FME-7 / 5B | Sunsoft-4: CHR-ROM nametables. 69: 5B PSG |
| 71 | Camerica / Codemasters | |
| 73, 75 | VRC3, VRC1 | |
| 76, 206 | Namco 3446, DxROM / Namco 108 | |
| 78 | Irem 74HC161 | Submapper 1 = one-screen, 3 = Holy Diver |
| 79 | NINA-03/06 | |
| 80, 82 | Taito X1-005, X1-017 | Internal RAM with unlock codes |
| 85 | VRC7 | VRC7a/b register wiring (submappers 2/1). 6-channel FM |
| 87 | Jaleco J87 | |
| 142 | Kaiser KS7032 | SMB2J and other FDS conversions. VRC3-style IRQ, PRG-ROM at `$6000` |
| 210 | Namco 175 / 340 | Submappers 1 / 2 |

---

## Roadmap

- The Famicom Disk System (mapper 20), using your own `disksys.rom` BIOS dump.
- MMC3A IRQ behaviour.
- The SOROM save-RAM banking fix.
- About 30 more boards: 15, 30 (UNROM 512), 37, 41, 46, 47, 48, 70, 72, 77, 86, 88, 89, 92, 93, 95, 97, 107, 113, the Sachen boards, 152, 154, 155, 184, 207, 228 (Action 52) and 232 (Camerica Quattro).
- PAL and Dendy timing; the Zapper.

## Project layout

| Path | Contents |
|---|---|
| `src/nes.h`, `src/nes.c` | System glue: the buses, power-on and reset, console models, power-off, the palette, save states |
| `src/cpu.c` | 2A03 CPU core and DMA |
| `src/ppu.c` | 2C02 PPU |
| `src/apu.c` | 2A03 APU and the mixer |
| `src/cart.c` | The cartridge connector model and the built-in boards (NROM, MMC1, MMC2/MMC4, the MMC3 family, UxROM, CNROM, AxROM and other discrete boards) |
| `src/board.h`, `src/boards.c` | Board interface and registry |
| `src/boards_konami.c` | VRC1, VRC2/VRC4, VRC3, VRC6 with its audio, VRC7 with its FM audio |
| `src/boards_mmc5.c` | MMC5 with its audio |
| `src/boards_misc.c` | Sunsoft, Namco, Bandai, Jaleco, Irem, Taito, Tengen, NTDEC, Kaiser and other boards |
| `src/chips.h` | Sound chip and EEPROM state |
| `src/ppuview.c` | PPU viewer renderer |
| `win/main.c` | Windows front end (Win32, GDI, waveOut) |
| `tools/nessys_cli.c`, `tools/accuracycoin_run.c` | Command-line tools |
| `tests/mapper_test.c`, `tests/dma_trace.c` | Regression suites |
| `tests/roms/` | Self-test ROMs, built by `tests/make_test_roms.py` with the small 6502 assembler in `tests/asm6502.py` |
| `scoop/`, `package.ps1` | Release packaging; see [docs/RELEASING.md](docs/RELEASING.md) |

---

## Credits

- **Emulator design and programming:** Claude, Anthropic's AI model. That covers all of the code in this repository: the CPU, PPU and APU cores, the cartridge and connector simulation, every board and sound chip, the Windows front end, the tools and the tests.
- **Logo artwork and play testing:** [ThatOneSlipSlopIdiotGuy](https://github.com/ThatOneSlipSlopIdiotGuy)

### Hardware references

NESSYSINTEGER was built from public hardware documentation and test ROMs:

- [NESdev Wiki](https://www.nesdev.org/wiki/): documentation of the CPU, PPU, APU, DMA, mappers, cartridge connector and lockout chip.
- [AccuracyCoin](https://github.com/100thCoin/AccuracyCoin) by 100thCoin: the accuracy benchmark.
- blargg's NTSC PPU research, including the power-up palette dump.
- The VRC7's instrument ROM, dumped from the chip by Nuke.YKT.

---

NESSYSINTEGER is an independent project. It isn't affiliated with or endorsed by Nintendo. NES, Famicom and Nintendo are trademarks of Nintendo.
