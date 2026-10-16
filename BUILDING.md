# Building Evander's Music Box

One script, `tools/build.py`, builds everything:

| Command | Needs | Makes |
| --- | --- | --- |
| `test` | a C compiler | runs the host self-test (`src/platform/host.c`) |
| `check` | clang or zig | compile-checks the FM-1 port against Felucca's HAL, without the JieLi toolchain |
| `web` | clang (wasm32) or zig | `build/web/`: emulator (`emb.wasm`), installer, console |
| `fw` | JieLi pi32v2 toolchain, AC79 SDK, Docker on non-Linux | `build/fw/emb.fwsc`, `emb.bin`, `emb.json` |
| `all` | all of the above | `fw`, then `web` with the package placed where the installer looks for it |
| `wav SLOT SEC` | a C compiler | `build/scene-N.wav`: one Encounter scene, rendered |
| `shots` | a C compiler (Pillow is optional) | `build/shots/`: the boot, the pages, Encounter |

## 0. The source

```sh
git clone --recurse-submodules https://github.com/<you>/evanders-music-box.git
cd evanders-music-box
# already cloned without it:
git submodule update --init          # upstream/felucca, pinned to Felucca v1.6.1
```

Python 3.9 or newer.

## 1. Host and web: a C compiler

The script uses the first compiler it finds: `$CC` / `$WASM_CC`, then `clang` (with a wasm32 target for the
web), then `python3 -m ziglang cc`, then `gcc` (host only). The simplest choice on every OS is Zig: it is one
pip install, with clang and a wasm linker inside.

```sh
pip install ziglang
python3 tools/build.py test
python3 tools/build.py check
python3 tools/build.py web
python3 -m http.server -d build/web 8000      # http://localhost:8000/emu/
```

The emulator needs `http://` (AudioWorklet and `fetch` do not work from `file://`).

### WebAssembly flags

The core is freestanding C99 with no libc, so wasm needs no WASI: `src/platform/wasm.c` provides `memcpy` and
`memset`, and storage comes through two imports (`env.js_store` and `env.js_load`) that the AudioWorklet
supplies. `tools/build.py web` runs:

```sh
clang -target wasm32 -O2 -std=c99 -nostdlib -fvisibility=hidden \
      -Wl,--no-entry -Wl,--export-dynamic -Wl,-z,stack-size=131072 \
      -o build/web/emu/emb.wasm src/platform/wasm.c
```

(Zig is passed `-target wasm32-freestanding` instead.) **WASI SDK** (for example 27) works the same way, since
its clang is a wasm clang. The `--target=wasm32` flag overrides its WASI default, and `-nostdlib` keeps wasi-libc
out:

```sh
WASM_CC="$WASI_SDK_PATH/bin/clang" python3 tools/build.py web
```

## 2. The FM-1 firmware

The FM-1's JieLi AC791N (WL82) has dual `pi32v2` cores. JieLi's toolchain is **clang 4.0.1 for pi32v2**, built
for Linux x86-64 only. The build follows Felucca's (`upstream/felucca/BUILDING.md`) and uses the same pinned
SDK files.

### 2.1 The toolchain

```sh
sh upstream/felucca/tools/get_toolchain.sh            # -> ~/.jieli/toolchain (pi32v2/, common/)
export JIELI_TOOLCHAIN=~/.jieli/toolchain             # default; set it if you put it elsewhere
```

Windows users should run this inside WSL or a Linux container, because it is a tar.xz of Linux binaries.

### 2.2 The AC79 SDK (Apache-2.0)

The packager needs three files from it: the SPL `cpu/wl82/tools/uboot.boot`, plus `cfg_tool.bin` and
`cfg/eq_cfg_hw.bin`.

```sh
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
export AC79_SDK=~/fw-AC79_AIoT_SDK                    # default
```

The build compares them with the SHA-256s Felucca was tested with, and warns if they differ.

### 2.3 Where the tools run

| Host | How |
| --- | --- |
| Linux x86-64 | natively |
| macOS (Intel or Apple silicon), Windows | in Docker: each tool runs in a `linux/amd64` `debian:bookworm-slim` container (Rosetta on Apple silicon). Start Docker Desktop first. `JIELI_DOCKER=1` forces this on Linux too. |
| Windows, without Docker | clone and build inside WSL2 (Ubuntu): that is Linux x86-64 |

### 2.4 Build

```sh
python3 tools/build.py fw --version 1.0
```

The script runs these steps (paths relative to the repository; `cc` is `pi32v2/bin/clang -target pi32v2`):

```sh
# the update loader: Felucca's, unchanged (runs from RAM, writes the app area)
cc -c upstream/felucca/firmware/loader/crt0_ldr.S -o build/fw/crt0_ldr.o
cc -Os -ffunction-sections -fno-builtin -Wall -Wno-unused-function \
   -Iupstream/felucca/firmware/hal -Iupstream/felucca/firmware/src \
   -c upstream/felucca/firmware/loader/loader.c -o build/fw/loader.o
pi32v2/bin/ld --gc-sections -e _start -T upstream/felucca/firmware/loader/loader.ld \
   build/fw/crt0_ldr.o build/fw/loader.o -o build/fw/loader.elf
common/bin/objcopy -O binary -j .text build/fw/loader.elf build/fw/loader.bin   # then LZ4-wrapped: ota.bin

# the app: Felucca's start-up, vectors and ISR wrappers + one unit, src/platform/fm1.c
cc -c upstream/felucca/firmware/crt0.S        -o build/fw/crt0.o
cc -c upstream/felucca/firmware/hal/fm1_vec.S -o build/fw/fm1_vec.o
cc -c upstream/felucca/firmware/hal/fm1_isr.S -o build/fw/fm1_isr.o
cc -Os -ffunction-sections -fno-builtin -Wall -Wno-unused-function \
   -Iupstream/felucca/firmware/hal -Iupstream/felucca/firmware/src \
   -DFELUCCA_ID='"FM-1_777"' \
   -c src/platform/fm1.c -o build/fw/emb.o
pi32v2/bin/ld -T upstream/felucca/firmware/app.ld \
   build/fw/crt0.o build/fw/fm1_vec.o build/fw/fm1_isr.o build/fw/emb.o -o build/fw/emb.elf
common/bin/objcopy -O binary -j .text     build/fw/emb.elf build/fw/text.bin
common/bin/objcopy -O binary -j .ram_text build/fw/emb.elf build/fw/ram_text.bin
common/bin/objcopy -O binary -j .data     build/fw/emb.elf build/fw/data.bin
# emb.bin = .text, then .ram_text and .data at their load addresses (_rt_load, _data_load)
# emb.fwsc = fm1pkg_make.ufw(fm1pkg_make.flash_image(emb.bin, KEY), ota.bin, "FM-1_777")
```

### Linker constraints

These come from `upstream/felucca/firmware/app.ld`, which the build uses unchanged:

| Region | Address | Size | What |
| --- | --- | --- | --- |
| XIP | `0x02000120` | 0x8DFBC (≈ 568 KiB) | code and constants, executed in place from flash; the SPL jumps to its first byte |
| RAMTEXT | `0x01C00000` | 24 KiB | `.ram_text`: the flash driver, which must run while the flash is busy |
| RAM | `0x01C08000` | 72 KiB | `.data` and `.bss`: what the ISRs touch |
| POOL | `0x01C1A000` | 360 KiB | `.pool` (`EMB_POOL`): string delays, reverb lines, scenes, the frame buffer |
| NOINIT | `0x01C7C000` | ≈ 15 KiB | the boot guard and crash record (they survive resets) |

- The app must fit the stock app slot of **0x8DFBC bytes**. The build refuses anything bigger.
- Flash: the boot head `0x0000-0x3FFF` is never written. The app lives in `0x4000-0x93000`. Evander's Box keeps
  its data at `0x97000 + slot × 4 KiB` (22 sectors to `0xAD000`: settings, workbench, 16 scenes and a
  4-sector autosave ring), and updates stage at `0xE0000`. The platform hooks are `emb_hal_erase`, `emb_hal_write`
  (page writes only, which never silence the audio) and `emb_hal_load`.
- No runtime library is linked. The core uses no float, no heap, and no variable-width 64-bit shifts
  (`tools/build.py check` enforces the last one). Felucca's `libc.c` supplies `memcpy` and `memset`.

## 3. Release

```sh
python3 tools/build.py all            # the version comes from the VERSION file
```

This writes `build/web/firmware/emb.fwsc` and `firmware.json` (version, identity, SHA-256), which the
installer verifies before it offers *Install*. Publish `build/web/` as a static site (GitHub Pages works:
`.nojekyll` is included). Because Felucca is GPL-3.0-only, give the source with every binary.

## 4. Developing

- **The core** (`src/emb_*.c`) is one unit included by all three platforms. Add a check to
  `src/platform/host.c` when you add behaviour.
- **Listening:** `python3 tools/build.py wav 10 30` renders the battle. Load the WAV into an editor to judge
  levels and tails.
- **Seeing:** `python3 tools/build.py shots`.
- **New scenes:** edit the pattern strings in `src/emb_scenes.c`. The syntax is documented at the top of the
  file.
- **The protocol:** `emb_sysex` in `src/emb_ui.c`, mirrored by `web/embmidi.js`.
- **Panel ids:** matrix buttons and encoders map to labels in `BTN_ID` and `ENC_ID` in `src/platform/fm1.c`.
  These are Felucca's measured `PANEL_DEFAULT`. If a unit's knob turns the wrong way, fix it there.
