# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

ChameleonUltra is an RFID emulation/reading device built on the Nordic **nRF52840** SoC. This
repo contains three cooperating parts:

- `firmware/` — embedded C firmware for the device (nRF5 SDK + S140 SoftDevice, ARM GCC).
- `software/` — the host-side CLI: a Python client (`software/script/`) plus native C crypto
  tools (`software/src/`) used for MIFARE Classic key-recovery attacks.
- `hardware/`, `resource/`, `docs/` — board files, DFU signing key + flashing tools, docs.

> **This is a fork.** The working branch adds a custom **reader-read ("select") detection**
> feature for a locker application (see `docs/FLASHING_MACOS.md`). Two commands
> (`DATA_CMD_HF14A_GET_SELECT_COUNT` / `..._CLEAR_SELECT_COUNT`) count reader anticollision+SELECT
> completions against the emulated tag, so a UID-only reader (no Crypto1 auth) can be detected.
> Upstream is `RfidResearchGroup/ChameleonUltra`.

## Build & test commands

### Firmware (requires ARM toolchain + nRF SDK — easiest via Docker)

```bash
cd firmware
# Build the DFU packages for the Ultra (outputs to firmware/objects/*.zip and *.hex):
docker compose up --pull=always build-ultra     # or build-lite for the Lite board
# Only one device type builds at a time; artifacts land in firmware/objects/
```

`firmware/build.sh` is the actual build entrypoint run inside the container. It builds the
bootloader then the application (each is a plain `make -j`), then uses `nrfutil` + `mergehex`
to produce signed DFU zips (`ultra-dfu-app.zip`, `ultra-dfu-full.zip`) and merged hex images.
`CURRENT_DEVICE_TYPE` (`ultra`|`lite`) selects the target; it maps to `PROJECT_CHAMELEON_ULTRA`
/ `PROJECT_CHAMELEON_LITE` compile guards used throughout the firmware.

To build without Docker you need `arm-none-eabi-gcc` (12.2.rel1), the nRF5 SDK, and `nrfutil`;
point `GNU_INSTALL_ROOT` at your toolchain in `firmware/Makefile.defs` (do **not** commit that
path). Flashing on macOS is documented in `docs/FLASHING_MACOS.md`.

### CLI / client (Python — use `uv`)

```bash
cd software
uv sync --dev                                    # install deps + dev tools (ruff, pyrefly)

# Native crypto tools (needed for MIFARE nested/darkside/hardnested attacks):
cd src && mkdir -p out && cd out && cmake .. && cmake --build . --config Release
# The CLI looks for the compiled binaries in software/script/bin/

# Run the interactive CLI:
cd software/script && python chameleon_cli_main.py
```

### Lint & tests

```bash
cd software
uv run ruff check .          # lint (also `ruff format` to format)
uv run pyrefly check         # static type checking (Python 3.9+ type hints required)

cd script
python -m unittest tests.test_ultra          # run one test module
python -m unittest discover tests            # run all tests
```

CI (`.github/workflows/`) runs ruff + pyrefly on any `software/**` change, and builds firmware +
client for every push/PR. **Any user-facing change must add a line under `## [unreleased]` in
`CHANGELOG.md`** — a workflow reminds you if you forget.

## Architecture

### Host ⇄ device command protocol (the backbone)

Communication is a binary framed protocol carried over **USB CDC or BLE NUS** (the firmware
auto-selects the reply channel in `auto_response_data()`). A frame is
`SOF(0x11) | LRC | cmd(u16) | status(u16) | len(u16) | LRC | data | LRC` — see
`ChameleonCom.make_data_frame_bytes()` in `software/script/chameleon_com.py` and the parser in
`on_data_frame_received()` in `firmware/application/src/app_cmd.c`.

**Command IDs are the contract between firmware and host and MUST be kept in sync in two places:**
- Firmware: `firmware/application/src/data_cmd.h` (`#define DATA_CMD_*` numeric IDs).
- Host: `software/script/chameleon_enum.py` (`class Command(IntEnum)` — same numbers).

ID ranges: `1000-1999` device/config, `2000-2999` HF reader, `3000-3999` LF reader, plus higher
ranges for tag emulation get/set commands. `chameleon_enum.py` also mirrors the status codes in
`firmware/application/src/app_status.h` (`STATUS_*`).

### Firmware command dispatch

Every command is one row in `m_data_cmd_map[]` at the bottom of `app_cmd.c`:

```c
{ DATA_CMD_XXX, cmd_before, cmd_processor, cmd_after }
```

`on_data_frame_received()` looks up the row and runs `before → processor → after`; if any
`before`/`after` hook returns non-NULL it short-circuits with that response. Reader commands use
`before_reader_run` / `before_hf_reader_run` as the `before` hook to reject the command with
`STATUS_DEVICE_MODE_ERROR` unless the device is in reader mode (and to power the antenna on/off).
A processor returns a `data_frame_tx_t*` built with `data_frame_make(cmd, status, len, data)`.

**To add a command:** add the ID to `data_cmd.h`, write a `cmd_processor_*` function, register a
row in `m_data_cmd_map[]`, then mirror the ID in `chameleon_enum.py` and add a CLI wrapper.

### Firmware RFID structure

`firmware/application/src/rfid/` splits by role, then by frequency:
- `nfctag/` — **emulation** (device pretends to be a card). `hf/` = ISO14443A 13.56 MHz
  (MIFARE Classic `nfc_mf1.c`, MF0/NTAG `nfc_mf0_ntag.c`, ISO14443-4 `nfc_14a_4.c`).
  `lf/` = 125 kHz, with per-protocol modulators under `lf/protocols/` (em410x, hidprox, ioprox,
  pac, viking, jablotron, idteck) and shared signal utils under `lf/utils/` (manchester, psk1,
  fsk, diphase).
- `reader/` — **reading** real cards. `hf/` drives the RC522 (`rc522.c`) with the MIFARE attack
  toolbox (`mf1_toolbox.c`); `lf/` has the 125 kHz radio + per-protocol decoders.
- Top level: Crapto1/Crypto1 implementations (`mf1_crapto1.c`, `mf1_crypto1.c`) and CRC/parity/hex
  utils shared by both roles.

The device runs in one **mode** at a time — `DEVICE_MODE_TAG` (emulation) or `DEVICE_MODE_READER`
— managed in `app_main.c`. Emulation data lives in **8 slots**; slot/tag persistence and the tag
type enum are in `rfid/nfctag/tag_base_type.h`, `tag_emulation.c`, `tag_persistence.c`.

### Host CLI structure

`software/script/` layers cleanly:
- `chameleon_com.py` — transport + frame codec (`send_cmd_sync`, timeouts, threads).
- `chameleon_cmd.py` — one Python method per device command; `@expect_response(Status.X)`
  decorators assert the returned status.
- `chameleon_enum.py` — `Command` + `Status` enums (mirror of the firmware headers).
- `chameleon_cli_unit.py` — the actual commands. A `CLITree` (from `chameleon_utils.py`) is built
  with `root.subgroup(...)` / `@group.command("name")` decorators (e.g. `hf 14a scan`,
  `lf em 410x read`). Command classes derive from `BaseCLIUnit` →
  `DeviceRequiredUnit` → `ReaderRequiredUnit`, which enforce connection/mode preconditions.
- `chameleon_cli_main.py` — the prompt-toolkit REPL entrypoint.

`software/src/` is standalone C ported from Proxmark3 (crapto1, darkside, staticnested,
hardnested, mfkey*) compiled via `software/src/CMakeLists.txt` into the `bin/` tools the CLI shells
out to for offline key recovery.

## Conventions

- **Conventional commits** and **atomic PRs** (see `CONTRIBUTING.md`) — avoid force-pushes and
  squash-to-one-commit PRs; granular history is used for bisection.
- Python must pass ruff + pyrefly and use type hints (min Python 3.9; CI/dev pin 3.13).
- New dependencies go in **both** `software/pyproject.toml` and `software/uv.lock` (via `uv add`).
- Never modify `resource/dfu_key/chameleon.pem` — it's the key the stock bootloader trusts for DFU.
