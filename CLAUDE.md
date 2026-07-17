# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

See also: `README.md` (user-facing behavior of the web UI) and `AGENTS.md` (agent playbook; note it is a draft that contains meta commentary and some generic guidance).

## What this repo is

**md-browser** — the SidecarTridge Multi-device **File & Download Manager** microfirmware app. It is a UF2 image that runs on a Raspberry Pi Pico W (RP2040) plugged into the Multi-device cartridge slot of an Atari ST / STE / MegaST(E). On the Atari it shows a QR-code boot screen; the real functionality lives in a **web interface** served by the RP2040 over Wi-Fi:

- Browse/search the public Floppy Images Database and download images to the microSD card.
- Full microSD file manager (upload/download, rename, copy/move, delete, attributes, folders, upload-from-URL).
- Create blank `.st`/`.st.rw` images, convert between `.MSA` and `.ST`, and browse/edit *inside* ST images like folders.
- USB Mass Storage mode: the microSD mounts as a USB drive when VBUS is detected.

Public programming docs: <https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>.

## Build

Top-level build is driven by `build.sh` in the repo root:

```bash
# <board_type> = pico | pico_w | sidecartos_16mb   (pico_w is the real target)
# <build_type> = debug | release  (only controls DEBUG_MODE macro + dist filename — see gotchas)
# <app_uuid_key> = UUID4 identifying this app; use the dev UUID while iterating
./build.sh pico_w debug 44444444-4444-4444-8444-444444444444
```

`44444444-4444-4444-8444-444444444444` is the development UUID (also the CMake default when `APP_UUID_KEY` is unset). Artifacts land in `dist/`: `<UUID>-<version>.uf2` and `<UUID>.json` (from the `desc/app.json` template with UUID/MD5/version substituted).

Required host environment:
- ARM GNU toolchain (`arm-none-eabi-*`) on PATH or via `PICO_TOOLCHAIN_PATH`.
- **Perl** — CMake hard-fails without it (web-asset embedding).
- `atarist-toolkit-docker` (`stcmd`) for the m68k target. `stcmd` needs a PTY; from a non-TTY context (CI, sub-shells) export `STCMD_NO_TTY=1` (the release workflow does this; the build workflow strips `-it` from `/usr/local/bin/stcmd` instead). Without it the m68k build can fail silently and a stale `BOOT.BIN`/`target_firmware.h` survives — the RP firmware builds fine but the ST shows garbage.
- SDK paths auto-set from the repo submodules if unset: `PICO_SDK_PATH`, `PICO_EXTRAS_PATH`, `FATFS_SDK_PATH`.

Build flow (orchestrated by `build.sh`):
1. Copies `version.txt` into `rp/` and `target/`.
2. Atari ST target (`target/atarist/build.sh`): `stcmd make release` assembles `src/main.s` with VASM/vlink into `BOOT.BIN` (raw binary at `$FA0000`). A copy is padded to 64 KB (`FIRMWARE.IMG`; build aborts if larger) and `firmware.py` converts it to `rp/src/include/target_firmware.h` — a C byte array embedded in the RP firmware. **Any change under `target/` requires a root rebuild so this header stays in sync.**
3. RP firmware (`rp/build.sh`): re-inits and re-pins submodules (pico-sdk `2.2.0`, pico-extras `sdk-2.2.0`, fatfs-sdk `v3.6.2`), sed-patches the fatfs-sdk submodule to enable `FF_USE_CHMOD`, runs CMake + make, produces `rp/dist/rp-<board>.uf2`.
4. Root script computes the MD5 and produces the final `dist/` artifacts.

Flash for a quick test with `picotool load dist/<UUID>-<version>.uf2`.

### Build gotchas
- **The build dirties the working tree by design**: `rp/build.sh` runs `git submodule update --init --recursive`, re-pins the submodule SHAs, and sed-patches `fatfs-sdk/src/include/ffconf.h` (leaving the `fatfs-sdk` submodule modified and moving `ffconf.h.bak` to the repo root). Do not commit these side effects, and never "clean them up" with `git restore`/`git clean`/`git submodule update` without explicit user approval — the tree may also hold the user's in-progress work.
- **CMake always runs with `-DCMAKE_BUILD_TYPE=Debug`** regardless of the `<build_type>` argument (the `$BUILD_TYPE` line in `rp/build.sh` is intentionally commented out). `<build_type>` only sets the `DEBUG_MODE`/`_DEBUG` macro (release silences UART stdio) and the dist filename.
- FatFs config: the project-owned override `rp/src/ff/ffconf.h` wins over the submodule default via `target_include_directories(... BEFORE PRIVATE)` in `rp/src/CMakeLists.txt` (LFN, relative paths, chmod enabled). Edit that file, not the submodule (the submodule sed patch in `rp/build.sh` exists only because some fatfs-sdk translation units use the submodule's own copy).
- `rp/src/fsdata_srv.c` is **generated** (see Web UI below) — never hand-edit it.
- Downloads from the Floppy DB use plain HTTP: `FMANAGER_DOWNLOAD_HTTPS=0` in `rp/src/CMakeLists.txt`.

### CI / release
- `.github/workflows/build.yml` — builds `pico_w` Release on PR.
- `.github/workflows/release.yml` — on `v*` tags: builds, creates a GitHub Release with the UF2 + JSON (release notes pull the top of `CHANGELOG.md` up to the first `---`), and uploads the artifacts to `s3://atarist.sidecartridge.com/`.
- `make tag` tags HEAD with the contents of `version.txt` (already `v`-prefixed, e.g. `v2.0.0`) and pushes the tag, which triggers the release.

### Tests
There is no test suite. Verification is: build succeeds, UF2 boots on hardware, manual interaction with the web UI and the serial debug console (UART, debug builds only).

## Architecture

Two-target build: a small m68k assembly boot screen runs on the Atari, compiled into a ROM image, embedded as a C array in the RP2040 firmware, and served back to the Atari over the cartridge bus that the RP2040 emulates via PIO + DMA (`romemul.c` / `romemul.pio`). All the app logic (web server, SD, downloads, USB) runs on the RP2040.

### Atari ST side (`target/atarist/src/main.s` — the only m68k source)
The cartridge header requests init **after GEMDOS init, before floppy boot** (`$08000000 + pre_auto`). `pre_auto` copies its loop out of ROM into RAM just below screen memory (running from the emulated ROM while the RP writes to it is unstable), then loops every vsync:
1. Blits the framebuffer from the cartridge window to screen memory (low-res copies words; high-res upscales through the translation table at `TRANSTABLE`).
2. Reads the command word the RP maintains at `FRAMEBUFFER_ADDR + FRAMEBUFFER_SIZE` and dispatches: `CMD_RESET` → reset the machine, `CMD_BOOT_GEM` → `rts` (continue GEM boot), `CMD_NOP`/`CMD_TERMINAL` → poll the keyboard.
3. Keyboard: **ESC** sends the `APP_BOOSTER_START` command to the RP; **any other key** boots GEM.

The Atari sends commands to the RP by *reading* magic sequences in the **ROM3** address space (`ROMCMD_START_ADDR = $FB0000`, magic `$ABCD`) — see `send_sync` in `inc/sidecart_macros.s` and the shared routines in `inc/sidecart_functions.s`.

### Shared 64 KB cartridge window (`$FA0000`–`$FAFFFF`, RP-side mirror `ROM_IN_RAM` at `0x20030000`)
Both sides derive offsets from their own constants — m68k in `main.s`, RP in `rp/src/include/display.h` (`DISPLAY_BUFFER_OFFSET`, `DISPLAY_COMMAND_ADDRESS_OFFSET`) and `mngr.h` (`TERM_*`). **They are kept in sync by hand — if you change one side, change the other.** Never hard-code an address inside this window.

| ST address | Symbol | Purpose |
| --- | --- | --- |
| `$FA0000` | `ROM4_ADDR` | cartridge image (header + boot-screen code, 64 KB cap) |
| `$FA1000` | `TRANSTABLE` | high-res upscale translation table (RP generates it) |
| `$FA8000` | `FRAMEBUFFER_ADDR` | 8000-byte 320×200 monochrome framebuffer (`display.c` renders u8g2 output here) |
| `$FA9F40` | `FRAMEBUFFER_ADDR + FRAMEBUFFER_SIZE` | command word polled by the m68k (`SEND_COMMAND_TO_DISPLAY`, `DISPLAY_COMMAND_*` ↔ `CMD_*`) |
| `$FAF000` | `RANDOM_TOKEN_ADDR` / `TERM_RANDOM_TOKEN_OFFSET` | top 4 KB: random token, token seed at +4, 16×4 B shared variables at +64 |

### RP2040 side (`rp/src/`)
- `main.c` — clock/voltage, `gconfig_init` + `aconfig_init` (jumping to the **Booster** app via `reset_jump_to_booster()` on failure), copies `target_firmware` into `ROM_IN_RAM`, `init_romemul(NULL, mngr_dma_irq_handler_lookup, false)`, then `mngr_init()`/`mngr_loop()`. **Don't add features to `main.c`.**
- `mngr.c` — **the application core.** `mngr_init()` brings up network + SD, shows the QR screen (`display_mngr.c` + `qrcodegen/`), connects Wi-Fi (STA, 3 retries), starts the HTTP server, then runs the main loop: lwIP poll, protocol commands, USB mass-storage attach/detach on VBUS, the download state machine (`download_poll`), and background copy jobs (`copy_poll`). `APP_BOOSTER_START` (ESC on the ST, or `booster.cgi`) resets the Atari and jumps to Booster.
- Command path: the DMA IRQ handler (`mngr_dma_irq_handler_lookup`) detects ROM3 accesses and `tprotocol_parse` pushes parsed commands into a small ring buffer; `mngr_loop()` consumes them on the main loop. **Keep IRQ-context work minimal** (`__not_in_flash_func`, no printing, no heavy work) — handle commands in the loop.
- `mngr_httpd.c` (~3400 lines) — all web endpoints on top of lwIP httpd CGI/SSI: file ops (`ls`, `ren`, `del`, `mkdir`, `attr`, `folder`), chunked uploads (`upload_start/chunk/end/cancel`), downloads to browser (`download.cgi`) and FloppyDB/URL-to-SD downloads (`download_start/status/cancel`), copy/move jobs (`copy_*`, `move_start`), floppy tools (`floppy_convert`, `mkst`), in-image browsing (`img_ls/ren/del/copy_start/import_start`), and `booster.cgi`.
- Feature modules: `stfs.c` (FAT12/16 filesystem access *inside* `.ST` images), `floppy.c` (MSA↔ST conversion, blank image creation), `copy.c` (background copy/move job engine), `download.c` (download state machine over `httpc/`), `usb_mass.c` + `usb_descriptors.c` (TinyUSB MSC exposing the SD card), `sdcard.c`/`hw_config.c` (FatFs over SPI), `network.c` (CYW43, lwIP **poll mode**), `display*.c` + `u8g2/` (framebuffer rendering).
- Infrastructure: `gconfig.c`/`aconfig.c` (global vs per-app config in dedicated flash sectors, on top of `settings/`), `select.c` (SELECT button → reset / factory erase), `reset.c`, `blink.c`, `tprotocol.h`.

### Web UI (`rp/src/fs/`)
Plain HTML/SHTML/CSS (`index.html`, `fmanager_home.shtml`, `browser_home.shtml`, `json.shtml`, `error.shtml`, `styles.css`). At build time `external/generate_fsdata.pl` + a **modified** `makefsdata` (adds text/css headers) minify and embed them into `fsdata_srv.c`. CMake tracks `fs/*`, so editing an asset regenerates the file automatically; edit the assets, never the generated C.

### Memory layout (`rp/src/memmap_rp.ld`)

| Region | Origin | Length | Purpose |
| --- | --- | --- | --- |
| `FLASH` | `0x10000000` | 1024 K | App code |
| `ROM_TEMP` | `0x10100000` | 128 K | Scratch area for loaded ROMs |
| `BOOSTER_APP_FLASH` | `0x10120000` | 768 K | Reserved for the Booster app (never write from this app) |
| `CONFIG_FLASH` | `0x101E0000` | 120 K | 30 sectors of per-app config |
| `GLOBAL_LOOKUP_FLASH` | `0x101FE000` | 4 K | UUID → config-sector lookup |
| `GLOBAL_CONFIG_FLASH` | `0x101FF000` | 4 K | Global config |
| `RAM` | `0x20000000` | 192 K | General RAM (Booster-style split reclaims the upper 64 KB) |
| `ROM_IN_RAM` | `0x20030000` | 64 K | The shared cartridge window served to the Atari |

Core 0 owns flash writes (`PICO_FLASH_ASSUME_CORE0_SAFE=1`) and overclocks to 225 MHz at `VREG_VOLTAGE_1_10` — the PIO bus emulation depends on it.

### App identity
`CURRENT_APP_UUID_KEY` (from the `APP_UUID_KEY` env var at CMake time) must match the `uuid` in `desc/app.json`; it is the key into `GLOBAL_LOOKUP_FLASH` for this app's config sector. Mismatch → the app jumps to Booster.

## Editing guardrails

- **Never modify** `pico-sdk/`, `pico-extras/`, or `fatfs-sdk/` — pinned submodules, re-pinned on every build. FatFs config changes go in `rp/src/ff/ffconf.h`.
- Feature work starts in `mngr.c` / `mngr_httpd.c` (or a new module), not `main.c`.
- If you touch the protocol or anything the two sides share (commands, shared-window offsets, payload layouts), update **both** `rp/` and `target/atarist/` and rebuild via the root `build.sh` so `target_firmware.h` regenerates.
- Prefer static, bounded buffers; no dynamic allocation in hot paths; watch stack usage.
- Match the existing C style (`.clang-format` / `.clang-tidy`, wired into CMake when the binaries are on PATH).

---

## Working style

These behavioral guidelines bias toward caution over speed. For trivial tasks, use judgment.

### 1. Think before coding

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 2. Simplicity first

Minimum code that solves the problem. Nothing speculative.
- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### 3. Surgical changes

Touch only what you must. Clean up only your own mess.
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it — don't delete it.
- When your changes orphan an import/variable/function, remove it. Don't remove pre-existing dead code unless asked.

The test: every changed line should trace directly to the user's request.

### 4. Goal-driven execution

Define success criteria. Loop until verified.
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan with a verification check per step.

### 5. No AI attribution

Never add AI-tool attribution to commits, PR descriptions, code comments,
docs, or any other artifact. This means **no**:
- "Generated with Claude Code", "Co-authored by Claude", "Made with ChatGPT",
  or any similar phrasing.
- `Co-Authored-By: Claude …`, `Co-Authored-By: ChatGPT …`, or any other
  AI co-author trailer.
- "AI-assisted", "written with the help of an LLM", etc., as comments or
  changelog entries.

Write the message as the human author. Do not mention AI tools used to
produce the work.
