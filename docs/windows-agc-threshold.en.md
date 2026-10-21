# Windows AGC Loop and Adaptive Verify Threshold — Reverse-Engineering Notes (v3.7.1.1 recheck)

> **Status: settled by static analysis; porting landed (2026-10-04).**
> Cross-validated against both driver generations (acerdrv v3.7.1.1 Ghidra
> decompilation + the 2021 Animeshz archive, instruction-level comparison;
> the core algorithms are identical across versions). Items marked
> "inferred" are decompile semantics, not verified
> instruction-by-instruction. §2.4 (AGC loop) and §4.1 (second chance)
> are implemented in the driver and live-verified on this unit; §3.3
> (adaptive threshold) is argued non-portable. This is a companion to the
> protocol/engine documents and changes no previously settled conclusion.
> [中文版](windows-agc-threshold.md)

## 1. Method and material

- **v3.7.1.1 decompile export**: Ghidra 12.1.2 headless, `-import` +
  postScript `scripts/GhidraDumpAll.java` (dumps every function's
  decompilation plus the string table). EgisTouchFP0575.dll 702 functions,
  Engine 635, Sensor 260.
- **The main DLL decompiles incompletely under auto-analysis** (the known
  desync problem, see the methodology note in windows-engine-tables §1).
  Key functions were located by scanning for rip-relative references to
  anchor strings and then disassembled targeted with capstone. Anchors:
  `et5xx_fetch_dynamic_intensity` (RVA 0x3dbd0),
  `fp_tz_secure_set_power_off` (0x3d978), `et5xx_power_on_initialize`
  (0x3da80).
- **Cross-reference**: the 2021 decompile under
  `refs/EgisTec-EH575/findings/EgisTec-EH575/decompiled_source/ghidra/`
  (39034 lines for the main DLL). Functions were compared instruction-level
  and confirmed isomorphic across versions.
- Export artifacts (in /tmp, regenerable with the commands above) are not
  distributed with the repo; this document records every RVA and constant so
  the findings are independently checkable.

## 2. et5xx_fetch_dynamic_intensity: the per-frame AGC loop (top porting candidate)

### 2.1 Evidence

| Item | 2021 | v3.7.1.1 |
|---|---|---|
| Function | `FUN_180009b20` (egis_fp_common_5XX.c) | RVA 0x9490/0x9530 region (log lines 0x4d4/0x4e5) |
| Intensity table | 16 inline stack immediates | .rdata RVA 0x3dc10 (file offset 0x3d010), 16 entries, delimited by the following string `et_get_image_state` |
| Call sites | inside the `egis_fp_get_image.c` frame loop (once per frame, several sites) | same (string/line correspondence) |

The 16-level cumulative intensity table (identical in both versions; this
fixes the two garbled 2021 stack-dump entries to 0x9800/0xbe66):

```
0x0800 0x395c 0x42e1 0x4c66 0x508f 0x5f33 0x68cd 0x7266
0x7c00 0x859a 0x8e66 0x9800 0xa19a 0xab33 0xb4cd 0xbe66
```

Core v3.7.1.1 loop excerpt (0x94c4–0x94f7, maps 1:1 onto 2021):

```
movzx  edi, byte [rbp-0x40]        ; measured intensity b0
mov    cl, 0x12                    ; register 0x12 (table row index)
shl    edi, 0xa                    ; x1024
call   read_reg                    ; -> [rbp-0x41]
loop:  mov edx, [rbp + rax*4 - 0x21]  ; table[k]
       cmp edi, edx / jl exit
       sub edi, edx / dec ebx         ; walk the table down
```

### 2.2 Algorithm (pseudocode, semantic reading)

```
measure: write 62 2c 02 {00 57} (priming) -> read 63 67 03 -> b0,b1,b2
         (alternate path: ctx[0x20]==1 uses an image-statistics variant)

if (b0 == 0 && b1 == 0xff):            # both ends saturated
    read 60 12 -> k; if k > 1 write 61 12 k-1   # step the row index down
else:
    if (ctx->level == 0xff): read 60 0f -> ctx->level   # first run: adopt sensor value
    if (b1 == 0xff):                     # too bright -> lower intensity level
        x = b0 << 10; read 60 12 -> k
        while (x >= table[k] && ...): x -= table[k]; level--
    elif (b0 == 0):                     # too dark -> raise level (cap 0x40)
        x = (0xff - b1) << 10; read 60 12 -> k
        while (x >= table[k] && level < 0x40): x -= table[k]; level++
    if (level != ctx->level): write 61 0f level; ctx->level = level
```

Register semantics (behavioural, inferred): `0x0f` = intensity/gain level
(write); `0x12` = current table row index (read/write); `0x2c/0x2d` =
measurement priming; `0x67` = 3-byte intensity readout (read). **Byte 5 (XX)
of `71 45 06 00 XX 87 13 00 03` is the intensity value this loop maintains**
(the "dynamic intensity" of protocol §2, measured 0xb9/0xbe across sessions
on one unit).

### 2.3 Comparison with our driver

| Windows | Our driver today |
|---|---|
| Closed-loop 0x0f/0x12 adjustment every frame; dynamic `71` byte | `71` byte hardcoded 0xb9 (last SHUTDOWN_PACKETS entry, egis0575.h); AGC registers 61 0a/0b/0c are static table values |
| Exposure drift continuously corrected | Drift unmanaged -> after desensitization the watchdog re-runs the whole chain (seconds) |

**Mechanistic explanation of the desensitization (hypothesis, consistent with
protocol §8.6)**: the >10-minute coverage decay 50%→2–5% is exactly what
unmanaged exposure/intensity drift looks like; Windows combines <0.1% duty
cycle with per-frame AGC, so it never exposes the problem.

### 2.4 Implementation (landed 2026-10-04; EGIS0575_AGC=1 enables, default off)

Cheaper than the original sketch: the telemetry needs no new priming — the
capture tables (POST_CAL/POST_INIT) already contain the measurement triple
`63 2c 02 00 57` -> `60 2d` -> `62 67 03` (10-byte response, data at
resp[7..9]) plus `60 0f 03` (resp[5] = current intensity level), responses
the driver used to discard. The port:

- resp_cb parses the telemetry (b0/b1/level); the rail decision mirrors the
  DLL's branch structure (b0==0 && b1==0xff -> `60 12` row read then
  `61 12 k-1`; b1==0xff alone -> walk the table down; b0==0 alone -> walk
  up, cap 0x40); adjustments insert ahead of SM_INIT through the SM_AGC_*
  states and always degrade-continue on errors (never fail the action);
- **zero extra traffic on a healthy sensor**: the telemetry rides existing
  packets and no rail means no new transfers — the same shape as the DLL;
- live verification (2026-10-04, this unit, 20 s probe + 15-minute soak):
  telemetry stable mid-range (b0≈0x7e–0x80, b1≈0x85–0x88, level=0x20),
  zero adjustment writes, zero timeouts, healthy capture loop (3 warmup
  frames + normal idle polling);
- **the 71-block byte provenance stays open**: live b0≈0x7f does not match
  the pcap-proven 0xb9/0xbe, so the dynamic patch is a separate opt-in
  `EGIS0575_AGC_71=1` (default off; the static 0xb9 is the field-proven
  value);
- remaining: reproduce a rail condition on hardware and compare long-soak
  desensitization before considering a default flip.

Implementation pitfall (hit during bring-up): the normal tail of SM_INIT
used `fpi_ssm_next_state`, relying on state adjacency to reach SM_START —
inserting SM_AGC_* between them turned it into a millisecond-cadence
`61 0f` write loop. Fixed with an explicit
`fpi_ssm_jump_to_state (SM_START)`; mind the same when inserting states
there again.

## 3. Adaptive verify threshold (the 'AE' mechanism — anti-hammering hysteresis, not portable at current calibration)

### 3.1 Algorithm (v3.7.1.1 `FUN_1800067c0` == 2021 `FUN_180006250`, line-isomorphic)

Model-5 parameters (§5 table): base threshold `[0x15]` = 660, `[0x21]` = 735,
vote gate `[0x1a]` = 85.

```
score = core matcher output (FUN_18000bad0)
if (score > base):                        # pivot is base(660), NOT effective
    effective -= 80; floor = 735          # only unwinds back to the start
else:
    effective += 60; cap = base + base/2 = 990
# the post-update effective then feeds this verify's verdict
if (score > effective && ctx[2] < 85):
    -> second-chance rescue path (§4.1); success sets the match flag
```

**Directional key point (corrected on recheck, 2026-10-04)**: effective
lives in [735, 990] and **starts at its own floor, 735 > base 660** — the
day-one pass bar is stricter than the calibrated base, and −80 merely unwinds
failure-induced tightening back to the start; it never goes below 735. The
mechanism is **anti-hammering hysteresis** (repeated failures push the bar up
toward 990; strong scores relax it to 735), not a "gets more lenient with use"
FRR win. Corroborating: the 992xxx caller resets effective straight back to
735 on record switches (the resting point), and the traced successful verify
score of 930 sits inside [735, 990].

The "clean pass" branch (score > effective with sufficient ctx[2]) hides
behind the pipeline vtables (EngineAdapterVerifyFeatureSet → pipeline
object) and was not traced instruction-by-instruction; ctx[2] is the match
context's second metric (inferred to be a weighted vote count on the order
of Σ ceil((128−h)/16)), and 85 at minimum gates the rescue path. This open
point does not affect the §3.3 conclusion, which rests on the walk dynamics
alone.

### 3.2 'AE' blob persistence

The adaptation state (a cluster of short fields including base/effective
thresholds, one global instance) serializes into an encrypted registry blob
(header "AE"/0x4541, key DAT_180038248, 169959 bytes measured in the
EngineAdapter.txt trace: `Update(commit, size=169959)`). Without a blob the
model defaults apply. Our in-process feedback cache (comparison §6) is the
same mechanism, reduced.

### 3.3 Porting assessment: not portable at current calibration (verdict: don't)

A faithful transcription is only non-trivial when floor > pivot (Windows:
735 > 660). On our score scale:

- **floor = pivot = 335**: the walk is a no-op — under per-probe semantics a
  probe scoring > 335 first pulls effective back to the floor before its own
  verdict (equivalent to passing against fixed 335), and a probe scoring
  ≤ 335 could never pass anyway. Identical to the current rule;
- **floor > 335** (e.g. Windows's ratio 335×1.114 ≈ 372): probes in the
  (335, floor] band get rejected — and the 2026-09-13 recalibration put the
  confident-genuine minimum at exactly **349**, inside that band. FRR
  regresses outright;
- **floor < 335**: trades FAR for FRR, against the direction of the
  calibration (confident-impostor peak 288, gray zone 321 vs threshold 335).

The Windows mechanism relies on the large margins of its score structure (a
full tier of headroom between base 660 and the pass bar 735); our genuine
margin is 14 points — structurally incompatible. Unless wider datasets push
the confident-genuine lower bound out, keep the fixed 335 + two-frame agree
rule; for FRR, work the §4.1 second-chance path instead.

## 4. Minor fidelity items

1. **Low-votes second chance (implemented 2026-10-04, default on)**: retry
   once when the score passes but votes fall short. Windows uses rotation
   compensation (`FUN_18000efd0`, parameter [0x16]=5); our analogue is a
   re-match with doubled geometric tolerances (12 px / 24°,
   `EGIS0575_M_SC_*`) counting towards the agree floor in
   `probe_matches_gallery` — the best score still comes from the strict
   pass, so FAR stays bounded by the unchanged threshold. Offline replay
   (tools/egis0575-feedback-eval, 36 scorable runs): **rescued one genuine
   run** (verify-run-194308-5, best=403 passing but agree short,
   previously a false NO-MATCH) with **zero impostor flips**.
   `EGIS0575_SECOND_CHANCE=0` disables.
2. **Vote-weight quantization**: Windows weights translation-histogram votes
   as `ceil((128−h)/16)` (v3.7.1.1 `FUN_180009b20`@7492); we count equally
   and score Σ(128−h) separately. Minor; keeping as-is is fine.

## 5. Full model-5 engine parameter table (v3.7.1.1 `FUN_18000e4e0` defaults + case-5 overrides)

| offset (×4) | default | model 5 | meaning (inferred) |
|---|---|---|---|
| [0x00] | 16 | 16 | enrollment frame-count family (model-default layer of windows-enrollment §2) |
| [0x01] | 1000 | 1000 | quality score scale |
| [0x03] | 3 | 3 | — |
| [0x04] | 9 | 9 | — |
| [0x05] | 0x700 | 0x700 | — |
| [0x0f] | 0x288 (648) | 648 | secondary score gate |
| [0x11] | 0x400 (1024) | 1024 | — |
| [0x12] | 0x73 (115) | 115 | **Hamming budget** (= our EGIS0575_M_HAMMING_BUDGET ✓) |
| [0x13]/[0x14] | 7 / 8 | 7 / 8 | geometry tolerance family (px/angle) |
| [0x15] | 660 | **0x294 (660)** | verify base threshold |
| [0x16] | 5 | 5 | minimum consistent pairs (= EGIS0575_M_MIN_MATCHED ✓) |
| [0x17] | −1 | −1 | — |
| [0x18] | 0x18 (24) | 24 | — |
| [0x1a] | 100 | **0x55 (85)** | verify vote gate |
| [0x1b]–[0x1e] | 10/20/5/240 | same | redundancy/progress family (windows-enrollment §5) |
| [0x20] | 20 | 20 | — |
| [0x21] | — | **0x2df (735)** | effective-threshold initial/floor |
| [0x22] | 170000 | 170000 | second-chance quality gate (the 0x88-offset item of §4.1) |
| [0x26] | 2 | 2 | — |

(Non-zero/meaningful items only; the raw dump lives in the decompile export.)

## 6. Confirmed equivalents / not worth porting (new evidence from this pass)

| Windows implementation | Evidence | Conclusion |
|---|---|---|
| `fp_tz_secure_set_power_off` | v3.7.1.1 RVA 0xa074 (log line 0x244): reads 0x40 for mode -> `61 0c 22 / 61 0b 03 / 61 0a fc` (+ optional `61 09 43`) | **Not a deep power-off command** — it is the re-arm wind-down, matching the SHUTDOWN_PACKETS tail. The `98/90` family stays parked under P3 |
| Re-arm prelude | 0x9f4c (line 0x22e): `61 0a f4 / 61 0c 44 / 61 40 00` + 0x40 polling | matches CAL_PHASE_5/POST_RESET values; nothing to change |
| `DeviceIdleEnabled/DefaultIdleTimeout/ResumingDetectModeParameterTuningEnabled` | 2021 `FUN_180014280` (registry reads; idle wait-timeout + detect-parameter retune on resume) | equivalent to our three-tier duty cycling + re-arm (protocol §7 conclusion unchanged) |
| Quality gates | v3.7.1.1 `FUN_18001c230` (AcceptSampleData): iResult→RejectDetail 9/7, `count_image_type_identifier()!=3`→5 | matches windows-enrollment §3; its discriminator (`FUN_180004410`, resample + table lookup producing type/amp/score) is a dedicated implementation — our Stage-2 gate is hardware-validated, an equivalent optional |
| Sensor adapter | EgisTouchFPSensor0575.dll merely forwards SensorStatus (one WINBIO_SENSOR_REJECT site) | nothing new to port |

## 7. Evidence index

| Content | 2021 | v3.7.1.1 |
|---|---|---|
| AGC function | EgisTouchFP0575.c `FUN_180009b20` (from L7586) | RVA 0x9490/0x9530 (targeted capstone, §2.1 excerpt) |
| AGC call sites (frame loop) | same file L8516/8771/9045/9103 | string + line correspondence |
| Intensity table | stack immediates in the same function (16 entries) | .rdata 0x3dc10 (file 0x3d010) |
| Verify + adaptation | EgisTouchFPEngine0575.c `FUN_180006250` (from L6228) | `FUN_1800067c0` (export line 5116; −0x50/+0x3c @ 5162/5176) |
| 'AE' blob decrypt/writeback | same file L4970/5080 region | export line 3823 region (0x4541) |
| Vote quantization | — | `FUN_180009b20`@L7492 (engine export) |
| Second chance | tail of `FUN_180006250` | tail of `FUN_1800067c0` (`FUN_18000efd0`) |
| Power-off / re-arm | — (function name absent) | 0xa074 / 0x9f4c |
| model-5 parameter table | `FUN_180012d20` region | `FUN_18000e4e0` + case 5 @15079 |

The recheck surfaced no evidence contradicting any settled entry in
protocol.md / windows-engine-tables.md; the four-source protocol baseline
stands unchanged.
