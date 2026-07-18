# Building & Flashing ChameleonUltra Firmware on macOS

This guide covers building the firmware in **this repo** (which includes the custom
**reader-read detection** command — see below) and flashing it to a ChameleonUltra over USB
on macOS. It applies to both Intel and Apple-Silicon Macs.

> **What's custom in this branch (`spike/reader-select-detection`)**
> Two new commands expose a "the reader read my emulated card" signal that works even for
> **UID-only** readers (no Crypto1 auth):
> - `DATA_CMD_HF14A_GET_SELECT_COUNT (4042)` → `[count:u32 BE][uidLen:u8][uid…]`
> - `DATA_CMD_HF14A_CLEAR_SELECT_COUNT (4043)`
>
> The counter increments in `nfc_14a.c` each time a reader completes anticollision + SELECT
> against the emulated tag. See the companion plan in `mastery-locker-app/MIFARE_DETECTION_PLAN.md`.

---

## 0. One-time prerequisites

```bash
# Docker Desktop (used to build with the exact ARM toolchain + nRF SDK; no local toolchain needed)
brew install --cask docker          # then launch Docker Desktop once so the daemon is running

# nrfutil — Nordic's flashing tool, plus its device plugin
brew install nordic-nrfutil || curl -sLo /usr/local/bin/nrfutil \
  https://developer.nordicsemi.com/.pc-tools/nrfutil/x64-osx/nrfutil && chmod +x /usr/local/bin/nrfutil
nrfutil install device

# (only needed for the enter_dfu.py helper) Python pyserial
python3 -m pip install --user pyserial
```

> The build is **not** signed with a new key — it reuses the repo's `resource/dfu_key/chameleon.pem`,
> which the **stock ChameleonUltra bootloader already trusts**. That is why a self-built
> `ultra-dfu-app.zip` installs on a normal device. **Never modify that key** (see `resource/dfu_key/warning.txt`).

---

## 1. Build the firmware (Docker)

```bash
cd firmware
docker compose up --pull=always build-ultra      # use build-lite for a ChameleonLite
```

The image is `linux/amd64`; on Apple Silicon Docker runs it under emulation (slower, ~a few minutes,
but works). Artifacts land in `firmware/objects/`:

| File | Use |
|------|-----|
| `ultra-dfu-app.zip` | **application-only DFU package** — what you normally flash |
| `ultra-dfu-full.zip` | bootloader + softdevice + app (recovery / first-time) |
| `application.hex`, `fullimage.hex` | raw images (J-Link) |

A successful build prints the `nrfutil … pkg generate` lines and leaves the two `.zip` files in `objects/`.

---

## 2. Put the device into DFU (bootloader) mode

**Option A — helper script (device plugged in, powered on):**
```bash
cd firmware
python3 ../resource/tools/enter_dfu.py     # sends the DFU command over the serial port
```

**Option B — manual:** power the device **off**, hold button **B**, and plug in USB while holding it.
LEDs **4 & 5** blink = DFU mode.

Confirm macOS sees it in DFU (Nordic VID:PID `1915:521f`):
```bash
nrfutil device list
# or:
system_profiler SPUSBDataType | grep -iA3 -E 'chameleon|0x1915'
```

> The repo's `firmware/flash-dfu-*.sh` scripts use Linux `lsusb`; on macOS use the `nrfutil`
> commands below instead (or the GUI in §4).

---

## 3. Flash over USB (nrfutil)

```bash
cd firmware
nrfutil device program --firmware objects/ultra-dfu-app.zip --traits nordicDfu
```

The device reboots into the new application automatically. If you ever brick the app slot, repeat
with `objects/ultra-dfu-full.zip`.

---

## 4. Alternative — flash over BLE/USB with ChameleonUltraGUI (easiest on macOS)

1. Install **ChameleonUltraGUI** (https://github.com/GameTec-live/ChameleonUltraGUI).
2. Connect to the device (BLE or USB).
3. Use its **firmware update / DFU** action and select the built `ultra-dfu-app.zip`.
   The GUI handles entering DFU and transferring the package.

This avoids the macOS `lsusb`/serial-port details entirely.

---

## 5. Verify the custom commands are live

After flashing, with the device connected to the **mastery-locker-app** (branch
`spike/mifare-read-detection`):

1. Open a locker and let the app emulate the Mifare 1K key (it calls `clearReaderSelectCount`).
2. Tap the device to the locker reader.
3. The app polls `getReaderSelectCount()`; a value **> 0** confirms the reader read the card and the
   UI flips to "Đã mở tủ thành công / Ổ khoá đã đọc thẻ".

Quick manual check (Python CLI in `software/script/`, optional):
```bash
# In the CLI, with a Mifare 1K slot active and the device emulating, after tapping a reader:
#   send command 4043 (clear), tap, then 4042 (get) — the returned u32 should be non-zero.
```

---

## Troubleshooting

- **`docker compose` pull fails / slow on Apple Silicon** — ensure Docker Desktop is running and has
  Rosetta/QEMU emulation enabled (Settings → General → "Use Rosetta"). The build still works, just slowly.
- **`nrfutil device` not found** — run `nrfutil install device` (the `device` subcommand is a plugin).
- **Device not detected in DFU** — re-do §2 Option B (hold **B** while plugging in); some cables are
  power-only — use a data cable.
- **DFU rejected / signature error** — you modified `resource/dfu_key/`. Restore it; the stock
  bootloader only accepts packages signed with that key.
