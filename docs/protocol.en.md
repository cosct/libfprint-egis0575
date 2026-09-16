# EgisTec EH575 (1c7a:0575) USB Wire Protocol

> **Status: settled.** This document is the protocol baseline used to develop
> the EH575 driver. All entries are cross-validated across four sources;
> entries marked "verified on hardware" were confirmed in live sessions on
> 2026-09-12/13. The driver ships from
> [cosct/libfprint-egis0575](https://github.com/cosct/libfprint-egis0575)
> (the `libfprint/` subtree) · AUR package `libfprint-egis0575`.
> [中文版](protocol.md)

Four-source cross-validation: the topni1 fork (hardware-verified), the
Animeshz reverse-engineering archive (pcap + Ghidra), the championswimmer
EH577 project (2026-06 state), and python-egistec-eh575 (independent
implementation).

## 1. Transport layer

| Item | Value |
|---|---|
| Interface | #0 (vendor-specific 0xFF/0xFF/0x00) |
| Command endpoint | bulk OUT `0x01` |
| Response endpoint | bulk IN `0x82` |
| Interrupt endpoints | EH577 has `0x83`/`0x84`; **unused in a captured Windows session** (§7), safe to ignore |
| Transfer model | one write + one read (command → mirrored-length response), no control transfers |

## 2. Command and response format

- Commands start with ASCII `EGIS` (`45 47 49 53`) followed by
  "register-style" bytes (the CET300 command set; semantics settled
  2026-09-13 from the v3.7.1.1 UMDF driver EgisTouchFP0575.dll
  decompilation cross-checked against the 2021 on-hardware pcaps):
  - `60 aa` — single-byte register read / status query (data in `resp[5]`;
    this is the polling mechanism)
  - `61 aa vv` — single-byte register write (`61 0a/0b/0c` are AGC/exposure;
    values are computed at runtime)
  - `62 aa nn data` — register block write (data inline, length nn)
  - `63 aa nn …` — register block read / parameter block (9–18 bytes)
  - `71 …` — **detect-mode parameter block**: `71 45 06 00 XX 87 13 00 03`
    appears after the AGC register writes and right before the `60 01`
    finger-poll loop starts (the DLL's SetDetectModeParameters; byte 5 XX
    is a dynamic intensity value from et5xx_fetch_dynamic_intensity —
    measured 0xb9/0xbe across sessions on the same machine); the short
    block `71 02 02 01 0c` sits on the re-arm (POST_REPEAT) path
  - `72 hh ll` — **block read** (hh ll = big-endian byte count):
    `72 14 ec` reads 5356 bytes
  - `73 hh ll` — **block write** (hh ll = big-endian byte count):
    `73 14 ec` announces a 5356-byte write; the 7-byte response is the
    block-write header ack, then the host sends the data and receives a
    7-byte confirmation. **Not calibration-specific** — the short response
    of `73 14 ec` inside EH577's PRE_INIT has exactly this meaning (former
    §10 open item, resolved)
  - `97 00 00` — sensor reset (response must be `SIGE` with status byte
    == 1); Windows sends it only on the recovery path (once, in the
    vmware-5 session)
  - Present in the v3.7.1.1 DLL but never on the wire in the 2021 pcaps
    (version difference or spare paths): `80 sub nn` / `81 hh ll`
    (256-byte-granularity block reads), `90 00`, `98 00` (write-only),
    `99 01/02/04` (inline block-write variants sent as one transfer)
- Responses start with `SIGE` (`53 49 47 45`) and mirror the command length
  (exceptions below)
- Notable exception commands:
  - `64 14 ec` → read one image frame, **5356 bytes** (103×52)
  - `72 14 ec` → read the calibration block, **5356 bytes**
  - `73 14 ec` → block-write header (see above)
- Response status byte: for most commands `resp[5]` carries a
  status/progress value (see the polls in §4)
- Known erratum: the response to `63 01 02 0f 03` is **9 bytes** (topni1's
  table says 7; reading 7 overflows)

## 3. Image geometry (settled)

- Raw frame: 5356 bytes = 103 columns × 52 rows, row-major, 103-byte stride
- **EH575 has no dead columns** (settled 2026-09-12 by dataset analysis):
  over 600 frames, every one of the 103 columns showed 100% non-zero rate
  with cross-frame std 11–20. The 33 hard-zero dead columns of the EH577
  (whose true active area is 70×52) do **not** exist on the EH575; the full
  103-column width is usable directly
- Pixel polarity: fingerprint images use `FPI_IMAGE_COLORS_INVERTED`
- EH575 empty-frame signature: the full frame carries low-intensity content
  (all 5356 pixels in the 15–150 gray range), completely unlike EH577's
  "no-finger frame has only ~173 active pixels". The discriminating signal
  is coverage after warm-background subtraction; the raw-finger-pixel count
  serves only as a sanity floor (empty frames sit at ~5356 and a press
  *reduces* it — <5300 also feeds the health watchdog's weak-press
  heuristic). See §6 for the three-gate criterion

## 4. Initialization sequences

### A. Calibration flow (topni1's flow; **required** for EH575 imaging — production path)

```
A. Read calibration (fresh read on first open; cached host-side across closes afterwards — see §4C/§7)
   1. PHASE_1 (16 packets: register config + AGC settings)
   2. Poll 60 2d until resp[5]==0x05
   3. PHASE_3 (2 packets: 62 67 03 / 63 33 03 73 10 01)
   4. Poll 60 35 until resp[5]==0x00
   5. PHASE_5 (4 packets)
   6. 72 14 ec → read the 5356-byte calibration block
   Corrupt-block detection: if the tail has ≥100 identical bytes
   (observed as 0x3f), discard and retry
B. Reset + upload calibration
   7. PRE_RESET (3 packets, last is the 97 00 00 reset)
   8. Poll 60 00 until resp[5]!=0x00 (reset complete)
   9. POST_RESET (13 packets)
  10. 73 14 ec → write 5356 bytes of calibration data → read 7-byte ack
  11. POST_CALIBRATION (20 packets, last is 64 14 ec which also
      collects one warm-up frame)
```

### B. PRE_INIT/POST_INIT flow (EH577's route; **does not work** on EH575)

The Animeshz/championswimmer alternative: PRE_INIT (29 packets) →
POST_INIT (18 packets + frame). EH575 semantics: when POST_INIT[1]
(`60 01 fc`) answers `SIGE 01 01 01`, the device is asking for
pre-initialization — jump back and re-run PRE_INIT (this is EH575's
intended error handling).

**Verified on hardware, 2026-09-12**: on route B every command is answered
correctly and `64 14 ec` reliably returns 5356 bytes, but the frame content
is **all zeros** — calibration upload is a hard requirement for imaging on
EH575. topni1's calibration flow is essentially PRE_INIT's register writes
with the calibration acquisition split out and made explicit. Also
verified: sending bare `73 14 ec` (without the 5356-byte payload) yields a
507 ms all-zero response the first time and deadlocks the transport on the
second. In the driver, route B is kept only behind
`EGIS0575_SKIP_CALIBRATION=1` for A/B experiments. The Python
implementation has a third equivalent sequence (8-packet rearm).

### C. Actual Windows session order (2026-09-13 pcap review, vmware-1 lockscreen session)

A complete Windows session init is just **8 commands plus the 5356-byte
calibration upload**, then it enters the frame loop — **it does not run
route A's calibration chain (PHASE_1/3/5), never reads calibration (72
count: 0 in all sessions), and never resets (97 appears once, in a
recovery scenario)**:

```
1  60 00 00 / 60 01 00          wake-up probe
2  61 0a f4 / 61 0c 44 / 61 50 03   AGC/exposure registers (dynamic values)
3  73 14 ec + 5356 B OUT        upload the [cached] calibration block (once per session)
4  60 40 ec                     poll until the upload is digested
5  63 09 0b 83 24 … / 63 26 06 06 …  parameter blocks (frame-loop config)
6  61 23/24/20/21 …             four register writes
7  → REPEAT frame loop (632c→602d→6267→600f→632c→6000→64 14 ec)
```

**The truth about calibration persistence (former §10 open item,
resolved)**: Windows persists the 5356-byte calibration block **host-side**
(an EgisFP registry/database cache) and uploads it with `73 14 ec` every
session — the sensor NVM is not involved, there is no "burn" command, and
the 72 read count is 0 across all ten pcap sessions. After power-up the
sensor firmware runs its own built-in calibration (corroborated by the
DLL's strings: fp_tz_secure_pre_calibrate, tz_calibrate_dvr,
et5xx_calibrate_bad_pixel, Zone1/Zone2 bad-pixel statistics,
vdm hw/target mean); topni1's route A simply reads that result out (72)
and feeds it back explicitly. This driver originally followed topni1's shape (fresh read and upload per
open); as of v0.2.1 the calibration block is cached host-side: it survives
close (dropped by the watchdog/broken-check/dispose paths) and later opens
re-upload it directly via 73 14 ec, with the sensor-health watchdog as the
invalidation trigger (Windows-shaped; see §7). As of 2026-09-15 (template
v2) the block is additionally persisted inside the enrolled template:
fprintd's print storage takes over the role of Windows' registry cache, so
when the sensor-side read is invalidated (the post-session all-zero block
of §8 item 3) the template's copy is uploaded instead, recovering
instantly.

## 5. Capture loop

- Frame acquisition: PRE_FIRST_IMAGE (25 packets, first loop only) →
  `64 14 ec` frame → REPEAT (8 packets) + `64 14 ec` frame ×N →
  POST_REPEAT (9 packets)
- **The shutdown sequence must be sent when capture ends** (topni1's
  POST_REPEAT, including the AGC/exposure `71` family): it prevents the
  sensor from being stranded in continuous-capture mode where presses stop
  registering (reproduced twice on hardware)
- Windows' frame loop likewise uses REPEAT (8 packets), not POST_CAL
  (20 packets) — see §7

## 6. Finger presence

- Hardware presence: poll `60 01` and test `resp[5] > 0x03` per topni1 —
  **unreliable**. On hardware it never reacted to presses (1199 polls,
  always 0x01), and Windows does not rely on it either (§7)
- Software criteria, compared:
  - topni1: mean squared adjacent-pixel difference in the open interval
    (100, 1000)
  - championswimmer (adopted by this driver): a three-gate presence test
    after warm-background subtraction — coverage ≥18% AND intensity ≥10
    AND raw_finger_pixels ≥800 (EGIS0575_PRESENCE_MIN_*, egis0575.h).
    Validated on an EH575 dataset: empty frames peaked at 17% coverage vs
    finger frames bottoming at 19% — a clean margin (56 finger / 450 empty
    frames). Frame *usability* is a separate Stage-2 quality gate (after
    median denoise + stretch5: grain <6%, 0<minutiae<10, ridge >4000 —
    see the Stage-2 comment block in egis0575.h)
  - python: np.std > 31.0

## 7. Windows online behavior, observed (2026-09-13, vmware-0.pcap, 57 s session)

| Window | Bus behavior | Takeaway for the driver |
|---|---|---|
| 0 s | Init: calibration upload (5356 B OUT) + POST_CAL header (15 packets) | Calibration happens once per session |
| 10 s | 6-packet keepalive (60 00/01/40 66 + 61 0c/0b/0a = POST_REPEAT tail family) | Keeps the sensor armed while idle |
| 10–50 s | **Fully silent (zero polling)** | Windows never babysits the sensor; it only captures during UI sessions |
| 50–56 s | Auth session: REPEAT light loop at full speed (~500 packets/s, 8+1 per frame) | The frame loop is REPEAT (8), not POST_CAL (20) |

Conclusion: Windows keeps the sensor healthy via a **duty cycle below
0.1%** (full-speed capture only for a few seconds while the auth UI is
active). fprintd semantics require continuous finger-state reporting, so
"don't babysit" cannot be copied. This driver's adaptation: three-tier slow
frame polling (120 ms for 15 s after action start / finger events; 230 ms / ~7%
duty after finger activity, dropping to
500 ms / ~3% after 30 s of quiet) + the in-driver sensor-health watchdog
(10-minute prophylactic re-init + weak-press degradation detection that
automatically re-runs the calibration chain) + the host-side calibration
cache surviving close (Windows-shaped, §4C); hard hangs still use the
manual `scripts/reset-sensor.sh`.

## 8. Known pitfalls (all verified on hardware, 2026-09-12/13)

1. **Per-claim quota on 5356-byte bulk reads**: as reported for topni1.
   Verified countermeasure: after releasing and re-claiming the interface,
   **re-upload calibration** (POST_RESET → 73 14 ec → write 5356 B → ack →
   POST_CAL): 156 recycles in 20 s with zero timeouts. The per-claim
   budget was relaxed from 6 frames to 200; pushing harder (1865
   re-uploads in 3.5 min) hangs the firmware — see comparison.md §7
2. **Bulk endpoint FIFO residue desynchronizes the next session**: if a
   process exits mid-response, the next process times out on the very
   first PHASE_1 packet. Countermeasure: `USBDEVFS_RESET` soft reset
   (scripts/reset-sensor.sh; works with the uaccess ACL, no root needed)
3. Calibration-block reads can be corrupt (≥100 identical tail bytes,
   observed 0x3f; new form observed 2026-09-14/15: post-session reads
   return **all-zero blocks** = the firmware calibration RAM is invalidated
   and rebuilt by the firmware in the background over seconds-to-minutes
   (in-chain 97 soft resets do not speed it up; an in-driver USB port reset
   actually wedges the chain's polls for minutes, so plain waiting is the
   reliable heal)) — the driver auto-recovers (final design 2026-09-15:
   immediate re-read to absorb transient corruption → the v2 template's
   embedded calibration copy runs the full open chain (register phases +
   97 reset + upload, with a structure self-check on the first warmup
   frame that falls back to re-reads on failure) — the Windows host-side
   cache carried through fprintd storage → without an embedded copy,
   patient 1 Hz re-reads until the firmware rebuild finishes;
   phase-poll non-convergence (seen after rapid session clusters,
   2026-09-16) joins the same retry chain, failing only after 7
   recoveries; every read logs its head/tail
   bytes to distinguish "FIFO-residue frame data" from "firmware-side
   invalid block")
4. No calibration → all-zero frames (§4B); bare `73 14 ec` deadlocks the
   transport on the second attempt
5. `63 01 02 0f 03` responds with 9 bytes (§2)
6. **Progressive desensitization under long continuous polling**: after
   >10-minute-scale continuous polling, press coverage decays from 50% to
   2–5% — the in-driver watchdog recovers automatically (10-minute
   prophylactic re-init + weak-press detection re-running the calibration
   chain); hard hangs use the manual `USBDEVFS_RESET`

## 9. System-level acceptance (2026-09-13 00:39)

With the sensor healthy, the full fprintd chain: `probes=1
best_score=1086/300 => MATCH` (pre-fix engine + old threshold 300 —
the current threshold is 335; see windows-engine-tables §7),
coverage 51%. Early-exit verdict (~2 s),
multi-probe collection, and the dual-frame agreement rule all work (see
windows-engine-tables.md for the matcher and its threshold calibration).

## 10. Not yet reverse-engineered

As of 2026-09-13 the three former open items are resolved (see the §2
command table and §4C): `73 14 ec` = generic block-write header
(length-parameterized); calibration persistence = host-side cache +
per-session 73 upload (no sensor-side burn); `71 …` = detect-mode
parameter block (with a dynamic intensity value), `97 00 00` = reset
(ack status byte must be 1).

Current open items:
- The `80`/`81`/`90`/`98`/`99 xx` command family exists in the v3.7.1.1
  DLL but never appeared in the 2021 pcaps — semantics inferred from the
  decompilation (§2), on-hardware behavior unobserved
- The register address map (0x2d/0x35/0x40/0x50/0x67/0x0a/0x0b/0x0c …)
  remains a black box: behavior known, hardware meaning unknown; would
  need the unpublished CET300 datasheet
- The internal layout of the 5356-byte calibration block (Zone1/Zone2
  bad-pixel tables + VDM mean parameters, inferred from DLL strings) —
  treating it as an opaque blob is sufficient for us

(The interrupt-endpoint 0x83/0x84 question was answered by §7: Windows
does not use them; the DLL contains EGIS_WAIT_INTERRUPT /
EGIS_TZ_STATE_NOTIFY_FINGER_DOWN strings, presumably firmware
capabilities of non-USB variants or left unenabled.)

## Reference implementation index

| Implementation | Path | Notes |
|---|---|---|
| topni1 (EH575) | `refs/topni1-libfprint/libfprint/drivers/egis0575.{c,h}` | Complete calibration flow, swipe model |
| championswimmer (EH577, authoritative b19955e) | `refs/libfprint-eh577/refs/libfprint/libfprint/drivers/egis0577.{c,h}` | Press-architecture base, 70×52 |
| Animeshz original patch (2021) | `refs/EgisTec-EH575/libfprint.patch` | pcap + Ghidra in `findings/` of the same repo |
| python standalone | `refs/python-egistec-eh575/open-fprintd-eh575/egis_driver/egis_driver.py` | Cross-validates sequence equivalence |

Reference repos are not distributed with this repository (see the root
README); fetch them at the paths above.
