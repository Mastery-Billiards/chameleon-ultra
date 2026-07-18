# Building & Flashing ChameleonUltra Firmware on macOS

A step-by-step guide to build the custom firmware in **this repo** (with the **reader-read
detection** commands) and install it on a ChameleonUltra over USB. Works on Intel and Apple-Silicon
Macs.

> **What's custom here (`spike/reader-select-detection`)**
> Commands that expose a "the reader read my emulated card" signal, for both HF and LF lockers:
> - **HF** (works even for **UID-only** readers, no Crypto1 auth):
>   `DATA_CMD_HF14A_GET_SELECT_COUNT (4042)` → `[count:u32 BE][uidLen:u8][uid…]`,
>   `DATA_CMD_HF14A_CLEAR_SELECT_COUNT (4043)`
> - **LF** (125 kHz field-detect, for EM410X lockers):
>   `DATA_CMD_LF_GET_FIELD_COUNT (5014)` → `[count:u32 BE]`,
>   `DATA_CMD_LF_CLEAR_FIELD_COUNT (5015)`
>
> Full background: `docs/READER_DETECTION_SUMMARY.md` and `mastery-locker-app/MIFARE_DETECTION_PLAN.md`.

---

## TL;DR (the whole thing, once prerequisites are installed)

```bash
cd firmware
docker compose up --pull=always build-ultra          # 1. build  → objects/ultra-dfu-app.zip
python3 ../resource/tools/enter_dfu.py               # 2. put device in DFU mode (must be plugged in)
nrfutil device program --firmware objects/ultra-dfu-app.zip --traits nordicDfu   # 3. flash
```

If you'd rather click than type, skip to **§5 (ChameleonUltraGUI)** — it's the easiest path on macOS.

---

## Step 0 — Install the tools (one time)

Run each block and check it succeeds before moving on.

**a) Docker Desktop** (builds with the exact ARM toolchain + nRF SDK — no local toolchain needed):
```bash
brew install --cask docker
open -a Docker            # launch it once; wait until the whale icon says "Docker Desktop is running"
docker info               # ✅ should print server info, not an error
```

**b) nrfutil** (Nordic's flashing tool) **and its `device` plugin:**
```bash
brew install nordic-nrfutil || { curl -sLo /usr/local/bin/nrfutil \
  https://developer.nordicsemi.com/.pc-tools/nrfutil/x64-osx/nrfutil && chmod +x /usr/local/bin/nrfutil; }
nrfutil install device
nrfutil device --version   # ✅ should print a version, not "unknown command"
```

**c) Python pyserial** (only for the `enter_dfu.py` helper in Step 2):
```bash
python3 -m pip install --user pyserial
```

> **About signing:** the build reuses `resource/dfu_key/chameleon.pem`, which the **stock
> ChameleonUltra bootloader already trusts** — that's why a self-built package installs on a normal
> device. **Never modify `resource/dfu_key/`** (see `resource/dfu_key/warning.txt`), or the bootloader
> will reject the package.

---

## Step 1 — Build the firmware

```bash
cd firmware
docker compose up --pull=always build-ultra      # use build-lite for a ChameleonLite
```

- On **Apple Silicon** the image runs under emulation → expect **a few minutes**. That's normal.
- ✅ **Success looks like:** several `nrfutil … pkg generate` lines, then the container exits `0`.
- ✅ **Check the output files:**
  ```bash
  ls -1 objects/*.zip
  # objects/ultra-dfu-app.zip     ← this is what you flash
  # objects/ultra-dfu-full.zip    ← recovery image (bootloader + softdevice + app)
  ```

| File | When to use |
|------|-------------|
| `ultra-dfu-app.zip` | **Normal install** — application only. Use this. |
| `ultra-dfu-full.zip` | Recovery / first-time — only if the app slot is bricked. |

---

## Step 2 — Put the device into DFU (bootloader) mode

Your ChameleonUltra shows up on USB in one of two modes:

| Mode | USB VID:PID | Meaning |
|------|-------------|---------|
| Normal | `6868:8686` | running the app |
| **DFU** | `1915:521f` | bootloader, ready to flash |

**Option A — helper script (device plugged in and powered on):**
```bash
cd firmware
python3 ../resource/tools/enter_dfu.py
# ✅ prints nothing / exits 0 and the device reboots into DFU
#    ("Chameleon already in DFU mode" is also fine)
```

**Option B — manual (if the script says "Chameleon not found"):**
1. Power the device **off**.
2. **Hold button B**, and while holding it, plug in the USB cable.
3. **LEDs 4 & 5 blink** = you're in DFU mode. Release the button.

**Confirm macOS sees it in DFU:**
```bash
nrfutil device list
# ✅ should list a device; or:
system_profiler SPUSBDataType | grep -iA3 -E 'chameleon|0x1915'
```

> ⚠️ The repo's `firmware/flash-dfu-*.sh` scripts detect the device with Linux `lsusb`, which isn't on
> macOS. Use the `nrfutil` command in Step 3 instead.

---

## Step 3 — Flash over USB

```bash
cd firmware
nrfutil device program --firmware objects/ultra-dfu-app.zip --traits nordicDfu
```

- ✅ **Success:** a progress bar to 100%, then the device **reboots into the new app automatically**.
- If the app slot ever gets bricked, repeat Steps 2–3 with `objects/ultra-dfu-full.zip`.

---

## Step 4 — Verify the custom commands are live

**With the mastery-locker-app** (branch `spike/mifare-read-detection`):
1. Open a locker → the app emulates the Mifare 1K key (it calls `clearReaderSelectCount`) and shows
   "Đang chờ ổ khoá đọc thẻ…".
2. Tap the device to the locker reader.
3. Within ~1 s the UI flips to **"Đã mở tủ thành công / Ổ khoá đã đọc thẻ"** (the poll saw
   `getReaderSelectCount() > 0`). If the device runs stock firmware instead, the app now reports
   **incompatible firmware** rather than silently failing.

**Optional CLI sanity check** (`software/script/`, device emulating a Mifare 1K slot):
send command **`4043`** (clear) → tap a reader → send **`4042`** (get); the returned `u32` count
should be **non-zero**.

---

## Step 5 — Alternative: flash with ChameleonUltraGUI (easiest on macOS)

Avoids all the USB/serial details:
1. Install **ChameleonUltraGUI** — https://github.com/GameTec-live/ChameleonUltraGUI
2. Connect to the device (BLE or USB).
3. Use its **firmware update / DFU** action and select your built `objects/ultra-dfu-app.zip`.
   The GUI enters DFU and transfers the package for you.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `docker info` errors | Docker Desktop isn't running — open it and wait for "running". |
| Build slow on Apple Silicon | Normal (amd64 under emulation). Ensure Settings → General → "Use Rosetta" is on. |
| `nrfutil device` not found | Run `nrfutil install device` (it's a plugin). |
| `enter_dfu.py` → "Chameleon not found" | Use a **data** USB cable (not power-only); or use Step 2 Option B. |
| Device not seen in DFU | Redo Step 2 Option B (hold **B** while plugging in); look for LEDs 4 & 5 blinking. |
| DFU rejected / signature error | You changed `resource/dfu_key/` — restore it; the bootloader only accepts that key. |
| Flash finishes but app unchanged | You flashed while in normal mode — ensure DFU (`1915:521f`) first, then reflash. |
