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
  - `DATA_CMD_HF14A_GET_SELECT_COUNT (4042)` → `[count:u32 BE][uidLen:u8][uid…]`
  - `DATA_CMD_HF14A_CLEAR_SELECT_COUNT (4043)`
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
- **`data_cmd.h`** — command ids `4042` / `4043`.
- **`software/script/chameleon_enum.py`** — mirrored `4042` / `4043` for the reference CLI.
- **`docs/FLASHING_MACOS.md`** — build & flash guide.

## 5. App changes

- **`chameleon.dart`** — `getReaderSelectCount()` hardened against short payloads; new
  `supportsReaderSelectDetection()` (capability probe via `GET_DEVICE_CAPABILITIES 1035`); **PROTO-1**
  parser self-heal (reset `dataBuffer`/`dataPosition` on framing/LRC error). SEC-3 log redaction
  extended to `mf1SetAntiCollision` (the HF path now carrying the UID).
- **`app_provider.dart`** — `unlockLocker()` emulates MFC1K, **probes for custom firmware up front**
  (fails loudly instead of silently expiring), then waits for the reader tap; keeps the FLOW-10 fix
  (COMPLETED deferred to `_onReaderRead`, not marked right after emulating).
- **`definitions.dart`** — `hf14aGetSelectCount(4042)` / `hf14aClearSelectCount(4043)`.
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
