# Reader-Read Detection — Work Summary

A record of the design, changes, review, and reconciliation done for the "confirm a locker
actually read the emulated card" feature, across the firmware and the companion Android app.

- **Firmware repo:** `chameleon-ultra-firmware` @ `spike/reader-select-detection`
- **App repo:** `mastery-locker-app` @ `spike/mifare-read-detection`
- **Companion design doc:** `mastery-locker-app/MIFARE_DETECTION_PLAN.md`
- **Flashing guide:** `chameleon-ultra-firmware/docs/FLASHING_MACOS.md`

---

## 1. The problem & the goal

The app uses a ChameleonUltra to emulate a locker key — either **LF EM410X (125 kHz)** (the primary /
current production case) or **HF Mifare Classic 1K (ISO14443-A)** — whose id/UID unlocks a
**UID-only** locker. A passive tag is never told "you were accepted", so the most we can observe is
**that a reader interacted with the emulated card** — which we use as the proxy for a successful tap.
The app then marks the request `COMPLETED` only on that device-confirmed read. Both frequencies are
supported; see **§10** for the LF path.

Stock firmware can't provide this: its only Mifare detection is the **mfkey32 log**, which records
**Crypto1 authentications only**. UID-only lockers never authenticate, so nothing fires. The fix is a
small custom firmware signal.

## 2. How it works (end to end)

```
Reader → REQA → ATQA → anticollision → SELECT(UID) → [tag answers SAK, goes ACTIVE]  ← counter++ here
                                                          ↑ works with no authentication
App: clearReaderSelectCount() → emulate MFC1K → poll getReaderSelectCount() every 700ms
     count > 0  → lockerOpened = true → PUT status=COMPLETED  ✅
     window expires → expireReaderTapWindow() → PUT status=FAILED
```

- The firmware increments a counter at the exact SELECT-completion point in `nfc_14a.c` (right after
  it transmits SAK and enters `NFC_TAG_STATE_14A_ACTIVE`). Generic to any HF tag type; fires for
  UID-only readers.
- Exposed via two poll commands (the app clears, then polls):
  - `DATA_CMD_HF14A_GET_SELECT_COUNT (4045)` → `[count:u32 BE][uidLen:u8][uid…]`
  - `DATA_CMD_HF14A_CLEAR_SELECT_COUNT (4046)`
- Design decision: **poll, not push.** The app's BLE transport has no unsolicited-frame dispatch path,
  and pushing one would risk the PROTO-2 stale-frame bug. Poll at 700 ms gives sub-second latency.

## 3. Firmware review — what was found

An adversarial review of the original spike surfaced (independently verified):

| Sev | Finding | Status |
|-----|---------|--------|
| 🔴 Critical | **Lite build broke** — the two command handlers sat inside `#if defined(PROJECT_CHAMELEON_ULTRA)` while their dispatch-table rows were unconditional, so the `lite` target referenced undefined functions. | **Fixed** |
| 🟡 Low | **Data races** — the last-UID buffer and `clear()` were read/written non-atomically across the NFCT interrupt (writer) and the main loop (reader); the response buffer wasn't zero-initialised (could ship uninitialised stack bytes / a torn UID). | **Fixed** |
| 🟡 Low | **No capability probe** — on stock firmware the app couldn't tell "command unsupported" from "no tap yet". | **Fixed (app side)** |
| ℹ Info | SELECT count increments multiple times per physical tap (harmless under the `count > 0` contract); UID filter can't discriminate other HF readers (single emulated UID). | Documented |

## 4. Firmware changes

- **`nfc_14a.c` / `nfc_14a.h`** — reader-select counter + last UID; new **atomic snapshot accessor**
  `nfc_tag_14a_reader_select_get()`; reads and `clear()` wrapped in `CRITICAL_REGION_ENTER/EXIT` so the
  NFCT-interrupt writer can't tear the multi-byte UID. (`#include "app_util_platform.h"` added.)
- **`app_cmd.c`** — the two `cmd_processor_*` handlers **relocated outside the `PROJECT_CHAMELEON_ULTRA`
  guard** (Lite fix); GET handler **zero-inits** its payload and fills it from the atomic snapshot.
- **`data_cmd.h`** — command ids `4045` / `4046` (renumbered from 4042/4043 when upstream claimed those for SEOS).
- **`software/script/chameleon_enum.py`** — mirrored `4045` / `4046` for the reference CLI.
- **`docs/FLASHING_MACOS.md`** — build & flash guide.

## 5. App changes

- **`chameleon.dart`** — `getReaderSelectCount()` hardened against short payloads; new
  `supportsReaderSelectDetection()` (capability probe via `GET_DEVICE_CAPABILITIES 1035`); **PROTO-1**
  parser self-heal (reset `dataBuffer`/`dataPosition` on framing/LRC error). SEC-3 log redaction
  extended to `mf1SetAntiCollision` (the HF path now carrying the UID).
- **`app_provider.dart`** — `unlockLocker()` emulates MFC1K, **probes for custom firmware up front**
  (fails loudly instead of silently expiring), then waits for the reader tap; keeps the FLOW-10 fix
  (COMPLETED deferred to `_onReaderRead`, not marked right after emulating).
- **`definitions.dart`** — `hf14aGetSelectCount(4045)` / `hf14aClearSelectCount(4046)`.
- **`open_locker_success_view.dart` / `locker.dart`** — waiting-for-tap UI.

## 6. Reconciling the app branch with `develop`

The spike branch was cut from an old base and was **4 commits behind `develop`**, which already
contained audit-driven fixes (**TR-1** RX-subscription lifecycle, **TR-3** `pendingConnection` reset,
**SEC-3** log redaction). Merging naively would have regressed them. We merged `develop` into the
branch and resolved conflicts to keep **both** the spike feature and develop's hardening:

- `unlockLocker()` — kept the spike MFC1K + reader-detection flow; folded in develop's **FLOW-1** UID
  validation; **adapted PROTO-4** to MFC1K UID lengths (develop's `5/13` was EM410X-specific);
  did **not** take develop's premature `completeOpenLockerRequest()`.
- Extended **SEC-3** to redact `mf1SetAntiCollision` (the spike moved the UID from the LF command to
  this HF command).
- `.gitignore` — kept both sides.

## 7. Verification status

| What | How | Result |
|------|-----|--------|
| Firmware compile-correctness | Adversarial review (preprocessor depth, symbols, macros, bounds, races) | ✅ clean; Lite now compiles |
| App merge correctness | Adversarial review (conflict residue, symbols, logic, hardening survived) | ✅ clean; only a pre-existing unused import (removed) |
| **Firmware actual build** | `docker compose up build-ultra && build-lite` | ⚠️ **not run here** (no Docker/toolchain) — **you must run it** |
| **App analyze/build** | `flutter analyze` + device build | ⚠️ **not run here** (no Flutter) — **you must run it** |
| End-to-end (tap a locker) | Manual, per `MIFARE_DETECTION_PLAN.md §4` | ⚠️ pending hardware |

## 8. Commits (local only — nothing pushed)

**Firmware** (`spike/reader-select-detection`):
```
474c8e5 chore: add CLAUDE.md guide and gitignore .idea
9d035e9 feat(hf): detect UID-only reader reads of the emulated tag
```
**App** (`spike/mifare-read-detection`):
```
10b49e4 chore: remove unused cupertino import in app_provider
e758bd9 Merge branch 'develop' into spike/mifare-read-detection
fb080aa feat(locker): confirm MFC1K unlock via reader-read detection
```

## 9. Known limitations & follow-ups

- **"Read" ≠ "bolt moved".** The signal proves the reader selected our UID — a strong proxy, but the
  card can't observe the lock mechanism. Mitigations: tight time window + physical presentation.
- **False positives from other HF readers.** Any HF reader (incl. a phone) that selects our one
  emulated UID increments the count; the UID filter can't discriminate. Accepted for UID-only lockers.
- **ATQA hardcoded 4-byte.** The MFC1K emulation uses `ATQA 00 04`; the UID guard allows 4/7/10 bytes,
  so a 7/10-byte UID would get an inconsistent ATQA. Harmless while the backend returns 4-byte UIDs.
- **Custom firmware is a hard dependency.** Devices must run this branch. Consider upstreaming the two
  commands or tracking the fork.
- **Push upgrade (future).** The firmware main-loop send path supports unsolicited event frames; once
  the app grows a keyed event-dispatch path (and PROTO-2 is fixed), detection could become push-based
  (immediate) instead of a 700 ms poll.
- **Next required step:** build both firmware targets in Docker and run `flutter analyze`, then do the
  end-to-end hardware test (both LF and HF).

---

## 10. LF (125 kHz / EM410X) extension

The HF SELECT counter only works for HF lockers. Since the **primary/production lockers are LF
EM410X**, we added an LF analog. LF is fundamentally different: EM410X has **no reader→tag handshake**
— the tag just modulates its id whenever a 125 kHz field is present — so there is nothing like a
SELECT to count, and no card identity is exchanged.

**What the LF signal is:** the firmware already senses "a 125 kHz field appeared" via the LPCOMP
comparator (`lpcomp_event_handler` in `lf_tag_em.c` — it drives the field LED / wake / EM410X
broadcast). We count that event and expose it to the host. Semantics: **"a reader energised the
antenna while we were broadcasting our id"** — weaker than HF (it can't confirm the reader
*decoded/accepted* the id, and carries no UID), but it's the strongest signal LF allows.

**Firmware changes:**
- `lf_tag_em.c` / `.h` — `m_lf_field_count` (volatile u32) incremented in `lpcomp_event_handler` on
  each field-up; accessors `lf_tag_em_field_count()` / `lf_tag_em_field_clear()`. A lone u32 is atomic
  on Cortex-M4 (no UID to tear), so no critical section is needed.
- `data_cmd.h` / `app_cmd.c` — `DATA_CMD_LF_GET_FIELD_COUNT (5014)` → `[count:u32 BE]`,
  `DATA_CMD_LF_CLEAR_FIELD_COUNT (5015)`. Handlers are outside the `PROJECT_CHAMELEON_ULTRA` guard
  (LF emulation exists on Lite too).
- `chameleon_enum.py` — mirrored `5014` / `5015`.

**App changes (choosing LF vs HF):** the unlock payload has **no card-type field** — `getLockerKeyData`
returns only `uid` + `name`, and every key enrolled today is LF EM410X (the only enrollment path).
So `unlockLocker()` picks the path by **UID length**: `5` bytes (or `13` for Electra) ⇒ **LF EM410X**;
`4/7/10` ⇒ **HF MFC1K**. These families don't overlap, so it's reliable for the two supported types.
The LF branch emulates EM410X, clears the LF field counter, and the poll reads `getLfFieldCount()`;
the HF branch is unchanged. `chameleon.dart` gains `getLfFieldCount` / `clearLfFieldCount` /
`supportsLfFieldDetection`; `_detectionIsLf` routes the poll and cleanup.

> **This also fixed a real bug:** the HF-only spike hard-coded MFC1K (HF) emulation for *every* key,
> so it emulated the **wrong frequency** for the (LF) production keys — and the merged UID guard
> `4/7/10` would have **rejected** a 5-byte EM410X UID outright.

**LF-specific caveats:** no card identity (can't confirm it was *your* card, only that a field
appeared); fires for **any** nearby 125 kHz reader; and the counter increments once per field
*appearance*. Mitigations are the same as HF (tight window + physical presentation).

**More robust future option:** have the backend return the key's card **type/frequency** in the unlock
payload so the app branches on that instead of a UID-length heuristic (the server already stores a
`tag` type string at enrollment; it just isn't echoed back on unlock).

---

## 11. LF coupling strength — why the field count was not enough

Section 10 listed "fires for **any** nearby 125 kHz reader" as a caveat. In the field it turned out
to be the whole problem, and it produced the worst failure this feature can have: **the app reported
a locker as opened while the door stayed shut.**

Two reproducible cases:

| Tap | What the device did | What the app said | What the locker did |
|-----|--------------------|-------------------|---------------------|
| Wrong position on the locker | White field LED flashed briefly, once | Opened | Stayed locked |
| Wrong face (the side without the A/B buttons) | White field LED lit for 1–2 s, once | Opened | Stayed locked |

### Root cause

`m_lf_field_count` is incremented from `lpcomp_event_handler`, which fires on an LPCOMP UP event.
LPCOMP is configured with `NRF_LPCOMP_REF_SUPPLY_1_16` — about **VDD/16, ~200 mV**. That is a
deliberately low bar, because it has to wake the device from a *weak* field. It is the wrong bar for
"the reader can read me": a reader whose carrier merely reaches the antenna trips it exactly as
readily as one close enough to demodulate the load modulation.

Coupling is reciprocal. The envelope amplitude the reader's carrier induces on our coil is the same
quantity that governs how strongly our load modulation appears back at the reader. A tap that
registers ~200 mV is, by construction, a tap the reader cannot decode — which is precisely why the
wrong-side case keeps dropping out and re-detecting: the envelope is hovering on top of the LPCOMP
reference.

### Why duration could not fix it

One emulation burst is `nrfx_pwm_simple_playback(..., 10, ...)` — ten repeats of a 64-entry sequence
at `counter_top = 64` on a 125 kHz base clock. That is **ten EM4100 frames, 32.768 ms each, ≈328 ms
per burst**. The 1–2 second wrong-side taps therefore pushed **30–60 complete frames** at the reader,
where a decode needs two or three consecutive clean ones. Those taps did not fail for want of frames
or time. They failed for want of signal, so no duration or frame-count rule can separate them.

### The fix: measure the envelope

`LF_RSSI` is `P0.02`, which is **`AIN0`** — the same pin LPCOMP compares is readable by the SAADC, and
it carries a peak-detected envelope (hence the existing "~2 ms time constant" drain comment). So the
firmware now *measures* what it used to merely threshold.

Sampling happens in `pwm_handler` immediately after `ANT_NO_MOD()` and the 2 ms settle — the one
moment per burst when `LF_RSSI` carries the reader's carrier and nothing of ours. Measuring there
costs no airtime and cannot disturb the id stream. It yields one sample per ~328 ms; that is coarse,
but sampling more often would mean shortening the burst, and a reader wanting consecutive clean
frames would start missing them.

**New commands** (`data_cmd.h`, `app_cmd.c`, mirrored in `chameleon_enum.py`):

- `DATA_CMD_LF_GET_FIELD_INFO (5016)` → 31 bytes, big-endian:
  `[count:u32][frames:u32][session_ms_max:u32][strong_ms_max:u32][rssi_last_mv:u16]`
  `[rssi_peak_mv:u16][samples:u16][strong_samples:u16][strong_run_max:u16][strong_mv:u16]`
  `[missed_samples:u16][flags:u8]`.
  `flags` bit 0 = emulating now, bit 1 = the envelope ADC channel was claimed,
  bit 2 = a sample hit ADC full scale.
- `DATA_CMD_LF_SET_STRONG_MV (5017)` ← `[mv:u16 BE]`.

Three details that are easy to get wrong and were:

- **A skipped conversion is not a weak sample.** `lf_rssi_sample_mv()` returns a
  `LF_RSSI_NO_READING` sentinel when the ADC is unavailable, and `lf_rssi_record()` leaves the run
  untouched rather than scoring 0mV. Scoring it weak would let a passing ADC conflict break the run
  of a perfectly good tap. Skips are counted in `missed_samples` so the condition stays visible.
- **`strong_ms_max` is measured in milliseconds, not samples.** A burst is ten frames, which is
  ~328ms for EM410X but ~655ms for Electra's double-length frame, so a sample count would quietly
  mean different things on different cards.
- **LPCOMP is disabled around the conversion.** `is_lf_field_exists()` leaves the comparator enabled
  on the same analog input, which loads the SAADC acquisition; the field check re-enables it, and no
  UP event is lost because they are ignored while emulating.

`5014`/`5015` still work; `5015` now clears the envelope statistics too.

The **threshold lives on the host** deliberately. Only the strong/weak split is judged on-device, and
even that is set over 5017, so a site whose readers run hot or cold is corrected without reflashing.

**Implementation notes.** SAADC channel 0 belongs to the battery monitor (`ble_main.c`), so LF sensing
claims channel 1 in `lf_sense_enable()` and releases it in `lf_sense_disable()`; a failed claim leaves
measurement off rather than asserting. LPCOMP, PWM and SAADC all run at `APP_IRQ_PRIORITY_LOW`, so
none preempts another and a blocking conversion inside a handler is safe. The multi-word snapshot is
read under `CRITICAL_REGION`, matching `nfc_tag_14a_reader_select_get`, and the command handler copies
it into a local before any `U16HTONS`/`U32HTONL` — those macros expand their argument repeatedly, the
same trap fixed in commit `914bfd9`.

### What the app requires now

A tap is accepted only when **both** hold:

- `adc_ok` — the device could measure at all. Without this a run of unmeasured samples would look
  identical to a reader that is never strong enough, i.e. a permanent silent refusal. The app checks
  it once at arm time and refuses loudly there instead.
- **one strong sample.** The device samples at the *end* of each burst, so a single strong sample
  already means ten EM4100 frames went into a field measured at or above the threshold. That is the
  whole claim being made, and a second sample adds nothing to it.

An earlier version demanded `strong_ms_max >= 300` — an unbroken stretch, i.e. two consecutive
strong samples — plus `session_ms_max >= 400`. It was reasoned as protection against a device swept
past a reader, and it caused the mirror-image failure in the field: lockers opened on the first burst
while the app kept asking for a tap that had already worked. Two things conspired. A stretch cannot
exist until a second sample lands ~330ms later, and a reader that drops its field the moment it
decodes never provides one — so `strong_ms_max` stayed at 0 over a door standing open.

The discrimination was never coming from the run length. It comes from the threshold, and with
3599 mV against 1784 mV there is enough daylight that one sample above the line is decisive.

The wrong-face tap clears the duration bar and is refused solely by the envelope threshold, so **that
number has to be right**.

### Calibrating the threshold (required, ~5 minutes, once per site)

The app ships a calibration tool: **Tài khoản → Đo tín hiệu đọc thẻ**.

1. Connect the device, press **BẮT ĐẦU PHÁT THẺ**.
2. Tap the locker three ways, holding ~2 s each, pressing **LƯU** after each:
   correct position (locker opens), wrong position, wrong face.
3. The screen shows the peak millivolts per scenario and proposes a threshold halfway between the
   strongest failing tap and the successful one. Press **LƯU NGƯỠNG**.

If a failing tap measures **as strong as** the successful one, the screen says so plainly instead of
proposing a number. That would mean envelope strength cannot separate these taps on this hardware,
and the measurements should be reported rather than worked around.

### Measured results (2026-08-24, real locker, real key)

Run on a ChameleonUltra over USB-OTG against a real locker reader, broadcasting **that locker's own
key**, so the correct tap genuinely opened the door and "strong reading" and "locker opened" were
confirmed together rather than one standing in for the other:

| Tap | Peak envelope | Strong stretch | Locker |
|-----|---------------|----------------|--------|
| Correct position | **3599 mV** | 8781 ms *(read the cautions)* | **opens** |
| Wrong position | **1784 mV** | 0 ms | stays shut |
| Wrong side (no A/B buttons) | **677 mV** | 0 ms | stays shut |

Both failing taps produced **zero** strong samples, so `strong_ms_max` stayed at 0 and the app
refused them — while the correct tap held a strong stretch of nearly nine seconds. The screen
proposed **2690 mV** (then the midpoint).

> **Two cautions about the top row.** Both were found later, and both matter, because this table
> gets cited for claims it cannot support.
>
> **The strong stretch was measured against a 600 mV bar, not today's 2300 mV.** This session ran
> the firmware default of the time; `ad55a35` raised it to 2690 and `3453646` settled on 2300, both
> *after* these numbers were taken — the screen "proposed 2690", i.e. it was computing a suggestion,
> not enforcing one. So "8781 ms unbroken" means only that the envelope never fell below **600 mV**
> across ~27 sample instants. It is **not** evidence that a correct tap stays above 2300 mV for nine
> seconds, and it does not rule out a carrier sag of up to 83%. Used as an argument against a
> bolt-actuation dip, it proves much less than it looks like it proves. What it does constrain is a
> *total* dropout: the run also ends on field loss, so no loss was seen at ~27 consecutive burst
> boundaries. Both checks sample once per ~330 ms, so anything shorter can fall between them.
>
> **3599 mV is the ceiling of the conversion, not a measurement.** At 14-bit resolution and 1/6 gain
> `raw * 3600 / 16384` cannot exceed 3599, and raw codes 16380-16383 all map to it. A correct tap
> sits on the rail with the clip flag one code away from tripping. The failing rows are mid-range and
> trustworthy; the good row is saturated, so **no amplitude change at good coupling can be seen in
> it**. That is why the loaded sample in §12 exists, and why the "saturation worry is answered"
> sentence this paragraph replaced was answering the wrong question: the failing taps are indeed
> nowhere near the rail, so the strong/weak *gap* is real — but the top of the range is still blind.

**The wrong-side tap is the whole argument in one line:** it raised **21 separate field detections**
and pushed **380 frames** at the reader — over a hundred times what a decode needs — and was never
once read. Frames and duration were never the scarce thing. Under the old rule, the *first* of those
21 detections would have reported the locker as opened.

**This also corrected two wrong defaults.** The first version shipped 600 mV, reasoned as
"comfortably above the ~200 mV LPCOMP trip"; the wrong-position tap reached **1784 mV**, so 600 mV
would have scored a failing tap as strong and left the bug exactly where it was. A second guess of
2000 mV left only ~200 mV of margin. The midpoint (2690 mV) came next and went too far the other
way: in use it refused taps that had already opened the locker, because a real tap does not always
couple as well as the one measured during calibration — a thicker faceplate, a different device, a
hurried hand all cost millivolts.

The default is now **2300 mV**, and the calibration screen suggests a threshold 30% of the way from
the worst failure to the good tap rather than 50%. That keeps ~500 mV of daylight above the failing
taps, which cluster because they are limited by geometry, and gives the rest of the range to genuine
taps, which vary. No amount of reasoning produced these numbers; only the measurements did — first
the separation, then the false refusals.

Note what the duration terms did *not* do. Both failing taps held 675 ms, clearing the 400 ms session
floor comfortably. Amplitude is doing all of the discriminating; the duration terms only refuse the
single-burst case.

### The escape hatch (do not remove it)

Everything above can be wrong in the quiet direction. A threshold calibrated against an unusually
close tap, a weaker reader elsewhere in the fleet, or a staff member who withdraws the instant the
bolt clicks all produce the mirror-image bug: a door that visibly opened, recorded as FAILED. Nobody
re-reads a FAILED record, so that mistake is *harder* to notice than the one being fixed.

So the tap screen carries a **TỦ ĐÃ MỞ** button behind a confirmation dialog, and
`expireReaderTapWindow()` takes one final reading before posting `/fail` — the device's counters are
cumulative, so a BLE stall that starves the poll loop must not discard evidence already on the
device.

The button is also the only honest route for an access resumed in `UNLOCKING`: locker-service will
not re-issue `/execute` from that state, so the key cannot be re-armed and there is nothing left to
measure. (That path previously set `waitingForReaderTap` without ever starting detection, so it
could only ever time out to FAILED.)

### Direction of failure

The two mistakes are not symmetric. A false success leaves a customer at a locked door with the
backend recording a completed unlock. A false failure means staff see an error next to a door that
did open, and retry. The thresholds therefore err strict, and the tap screen coaches
("Tín hiệu yếu — chạm mặt có nút A/B…") instead of silently waiting.

---

## 12. Actuation evidence — separating an accepted read from a refused one

§11 settled which taps a reader *could read*. It cannot settle which taps a reader *accepted*, and
those are different questions with a bug living in the gap between them.

### The failure

Staff hold tủ 2's key and tap tủ 3's reader. Tủ 3 energises, reads the id at full strength, refuses
it because it is not tủ 3's key, and never moves its bolt. Every field in the tap profile reads
exactly like a real open, so the app reported tủ 2 open. Downstream that is not recoverable:
`/success` flips the locker's asset status, writes an unlock record and sends **the customer** a
message about a locker that never opened, and `COMPLETED` is terminal — no endpoint can take it back.

**No threshold can fix this, and no amount of calibration can either.** Envelope amplitude is
reciprocal coupling: it is decided by how the device is held against the faceplate, and it is
identical whether the id was on that reader's list or not. Acceptance is a decision taken inside the
lock, on a protocol with no back-channel — EM410X is a one-way broadcast. Every earlier fix in this
document tuned the *read* question; this one is a different axis entirely.

### What could possibly differ

Only a side effect. An accepted read fires a bolt: a motor or solenoid draws roughly 0.3-3 A for
tens to hundreds of milliseconds out of the same cells that generate the 125 kHz carrier, and drags a
ferrous armature past the reader's coil. Both push the carrier **down**. A refusal drives a piezo at
a few tens of milliamps and moves nothing.

So the evidence is a **dip**, and two things had to change before one could be seen at all.

**Everything the firmware reported was a maximum or a count.** `rssi_peak_mv` only rises,
`strong_samples` only counts up, and a strong run that dips and recovers leaves `strong_ms_max`
untouched. A dip was invisible **by construction** — none of the measurements taken so far could have
detected one even if it happened. `rssi_min_mv` and `weak_run_max` are the first fields that can move
downward.

**The idle sample saturates exactly where the answer would live.** A correct tap reads 3599 mV, which
is the ceiling of the conversion (see the caution in §11), so there is no headroom in which a sag
could register. `pwm_handler` therefore takes a **second sample with the modulator held on**:
`ANT_MOD()`, 2 ms to settle, convert, `ANT_NO_MOD()`, 2 ms to recover, then the usual field check.
Load modulation damps the antenna, so the loaded envelope sits well below the unloaded one — back
inside the converter's range — while still tracking it monotonically. It costs no airtime: the burst
is already over, and it happens inside the inter-burst gap the idle sample already used. The gap
grows from ~2 ms to ~6 ms against a ~328 ms burst.

The host compares `rssi_loaded_min_mv` against `rssi_loaded_peak_mv` and treats a fall past a
configured fraction as evidence a bolt moved. `DATA_CMD_LF_GET_FIELD_INFO` grew from **31 to 39
bytes**, appended rather than interleaved, so an older host that stops reading at byte 31 parses
every field it knows exactly as before.

### Status: instrument first, and say so plainly

**The dip has never been measured on these readers.** It cannot have been: every calibration run so
far broadcast the locker's own key, so every strong read on record is an *accepted* one. A
refused-but-strong tap — the exact case the bug lives in — has never been captured.

The default bar (10% of the loaded peak) is a starting point taken from the physics, not from
hardware, and the app's calibration screen exists to replace it with a measured number. It gained a
fourth scenario, **"Tủ khác — đọc được nhưng KHÔNG mở"**, which is deliberately excluded from the
millivolt threshold maths: folding it into the failure ceiling would push the suggested threshold
above the good tap and report "no separation" every time, which is true of amplitude and useless as
advice. Compared against the correct tap it gives the actuation bar instead.

Run it with the locker's **real key**, so the correct tap genuinely opens the door, and take both
taps in one visit. Three outcomes:

- **The accepted tap dips and the refused one does not** — the bar is measurable, save it, and the
  wrong-locker case is refused automatically from then on.
- **Neither dips** — this lock's RF section is regulated separately from its motor, or it is
  mains-fed. No carrier measurement can separate the two taps on this hardware. The screen says so
  rather than proposing a number.
- **Both dip equally** — same conclusion, and the screen says that too.

The second and third outcomes are real possibilities, not edge cases. If either turns up, the honest
answer is that the RF channel cannot settle which door opened, and identity has to come from
somewhere outside it — a per-locker marker read before the key goes on the air is the option that
ends the class rather than inferring around it.

### Direction of failure, restated

Unchanged in principle and worth repeating, because this feature moves the balance: a tap that opened
a door and was recorded FAILED costs a retry, and staff see it immediately. A tap that opened nothing
and was recorded COMPLETED corrupts the locker's state, messages the wrong customer, and nobody ever
re-reads it. When the evidence is absent the flow waits and then fails; it does not guess.
