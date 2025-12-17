`AGENTS.md` is a “repo guide for coding agents” (like GPT-5.1-Code-Max): how to build, test, format, where the entry points are, and the project-specific gotchas. It’s basically a more actionable `CONTRIBUTING.md`, written for an automated teammate.

Below is a solid `AGENTS.md` tailored to `sidecartridge/md-browser`, based on the repo layout + README and the official SidecarTridge Multi-device programming docs. ([GitHub][1])

````md
# AGENTS.md

This repository contains the **SidecarTridge Multi-device File & Download Manager** (“md-browser”): a microfirmware app that runs on the Multi-device (RP2040/Pico W class) and exposes a **web interface** to:
- browse/search/download from the public Floppy Images Database
- manage files on the microSD (upload/download/rename/delete, folders, attributes)
- optionally use USB Mass Storage mode for the microSD

See `README.md` for user-facing behavior and UI flows.

---

## Project shape (important)

This codebase follows the standard SidecarTridge Multi-device microfirmware layout:

- `rp/`  
  Microcontroller-side code (RP2040). Owns hardware access, SD, network/web UI, protocol parsing, etc.

- `target/`  
  Target-computer side firmware (Atari ST tooling / scaffolding) that talks to the microcontroller over the ROM-based command protocol.

- Submodules at repo root (do not “vendor” these):
  - `pico-sdk/`
  - `pico-extras/`
  - `fatfs-sdk/`

- Root build orchestrator:
  - `build.sh` at repo root runs both `rp/build.sh` and `target/build.sh` and then stages artifacts into `dist/`.

- Tooling/config:
  - `.clang-format`, `.clang-tidy`, `.clang-tidy-ignore`
  - `.vscode/` (debugging presets)

**Key coupling:** the `target` build produces a binary that is converted into a C array and embedded into the RP project (typically under `rp/src/include/` as `target_firmware.h`). If you change anything in `target/`, you must rebuild so the embedded header stays in sync.

---

## Build prerequisites

### 1) Initialize submodules
```sh
git submodule init
git submodule update --init --recursive
````

### 2) Environment variables

Build scripts rely on these variables (they can point to the repo’s submodule paths):

* `PICO_SDK_PATH` (e.g. `$(pwd)/pico-sdk`)
* `PICO_EXTRAS_PATH` (e.g. `$(pwd)/pico-extras`)
* `FATFS_SDK_PATH` (e.g. `$(pwd)/fatfs-sdk`)

### 3) Toolchain

You need an ARM embedded toolchain (`arm-none-eabi-*`). VS Code + CMake Tools is recommended for debugging.

A Pico debug probe (picoprobe / Raspberry Pi Debug Probe) is strongly recommended for real debugging.

---

## Build commands (what agents should run)

Use the repo root `build.sh`:

### Debug build (recommended while iterating)

```sh
./build.sh pico_w debug 44444444-4444-4444-8444-444444444444
```

### Release build

```sh
./build.sh pico_w release <YOUR_UUID4>
```

Notes:

* `pico_w` is the current supported target device name in the build scripts.
* Use the special DEV UUID `44444444-4444-4444-8444-444444444444` during development.

### Expected artifacts

After a successful build, `dist/` should contain:

* `<UUID>.uf2` (microfirmware app)
* `<UUID>.json` (app metadata for Booster)
* `<UUID>.md5` (checksum for Booster)

---

## Flash / deploy (for quick validation)

You can load the UF2 with `picotool`:

```sh
picotool load dist/<UUID>.uf2
```

For serious work, prefer debugging with a probe + VS Code (see `.vscode/` and the SidecarTridge docs).

---

## Agent workflow rules (read carefully)

### Non-destructive workflow (must follow)

This repo is often edited interactively by a human while the agent is running. Treat the current filesystem state as the source of truth.

**Never discard or overwrite local changes without explicit user approval.** In particular, do not run any of these unless the user explicitly says to:

* `git restore` / `git checkout -- <path>` / `git reset` (any form that reverts files)
* `git clean` (any form)
* `git submodule update` / “pinning” submodules / changing submodule SHAs

Before running build scripts (especially root `./build.sh`), the agent must:

1. Warn that the build may touch multiple files/submodules and can create a lot of diffs.
2. Ask the user how to protect in-progress work: commit, stash, or proceed without protection.
3. After the build, show `git status --porcelain` and ask before cleaning up any diffs.

### Always keep `rp/` and `target/` in sync

If you:

* add/modify a protocol command
* change payload layouts
* change any embedded “target firmware” behavior

…you must update **both** sides and rebuild via root `build.sh` so the generated embedded header stays correct.

### Don’t do heavy work in IRQ/PIO/DMA handlers

Protocol parsing/lookup may happen in an interrupt context, but command handling should be pushed to the main loop. Keep interrupt handlers fast: capture state, set flags, return.

### Respect flash/RAM and “old computer” constraints

* Prefer static buffers and bounded sizes.
* Avoid dynamic allocation in hot paths.
* Be careful with stack usage (RP2040).
* Keep protocol payload sizes and memory map assumptions stable.

### Formatting / linting

* Follow `.clang-format` and `.clang-tidy` configurations in the repo root.
* If you touch C/C++ files, format them and avoid introducing new clang-tidy warnings.

### Submodules

Do not change submodule pins unless the change is explicitly required. If you do, call it out clearly in the PR/commit message and note compatibility risk.

### No secrets

Do not add tokens/keys/credentials to the repo. Keep config local.

---

## Where to make common changes

### Web UI / “app behavior”

Usually in `rp/` code (HTTP handlers, SD operations, network stack integration).

### File manager logic

Microcontroller side (`rp/`) owns filesystem operations; ensure operations are safe and return clear error codes back through the protocol/UI.

### Protocol changes

* Update the microcontroller “listener” implementation (C)
* Update the target “sender” implementation (C/ASM as appropriate)
* Rebuild to refresh embedded target firmware header

---

## What “done” looks like for a change

Before considering a change complete:

1. Root build succeeds:

   ```sh
   ./build.sh pico_w debug 44444444-4444-4444-8444-444444444444
   ```
2. `dist/` contains UF2/JSON/MD5 for that UUID.
3. If protocol/target changed: confirm the embedded header was regenerated (by doing a clean rebuild if unsure).
4. No obvious style regressions (clang-format) in files you touched.
5. Behavior matches the README expectations (QR boot screen behavior, ESC to Booster, “any key” to GEMDOS, etc.).

---

## Pointers (authoritative docs)

If anything here conflicts with official documentation, the docs win:

* SidecarTridge Multi-device Programming Guide (microfirmware architecture, build, protocol).

```

If you want, I can also add a **tiny “repo-specific” section** once GitHub stops erroring on the file blob views (so I can read `build.sh`, `rp/`, and `target/` specifics directly). Right now, the repo root/README and the official programming guide are readable, but GitHub’s blob pages are throwing “can’t perform that action” in this browsing environment.
::contentReference[oaicite:1]{index=1}
```

[1]: https://github.com/sidecartridge/md-browser "GitHub - sidecartridge/md-browser: Browse the internet floppy repositories and direct download"
