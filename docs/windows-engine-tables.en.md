# Windows Engine Coefficient-Table Extraction & Replica Notes (EgisTouchFPEngine0575.dll v3.7.1.1)

> **Status: complete.** All coefficient tables were extracted; the Python
> replica achieved perfect offline separation; the C port ships with the
> driver [cosct/libfprint-egis0575](https://github.com/cosct/libfprint-egis0575) (libfprint/ subtree)
> v0.2.1 (`egis0575-matcher.c`). This document is the full lab record,
> kept for review and future enhancements.
> [中文版](windows-engine-tables.md)

## 1. Source and method

- DLL source: `acerdrv/Fingerprint_EGISTEC_3.7.1.1_W10x64/x64/` (Acer's
  official driver package for the SFX14-41G). The engine DLL is 324 KB and
  differs from the version Animeshz decompiled → his addresses don't
  transfer; tables had to be relocated by signature scanning
- **Methodology breakthrough**: capstone's `disasm()` stalls on bad bytes,
  so the first pass missed 90% of the code (5,127 → 48,826 instructions
  once fixed); switching to **desynchronization-resynchronizing linear
  sweep** closed the reference chain
- Replica vehicles: `scripts/egis_matcher.py` (Python reference);
  `tools/egis0575-matcher-test.c` (C cross-validation harness)

## 2. Confirmed coefficient tables (extraction session 2026-09-13)

| Table | RVA | Verification |
|---|---|---|
| sin table, 360×int32 Q16 | 0x3eac0 | all 360 entries match sin(i°)×65536 (tolerance 8) |
| cos table, 360×int32 Q16 | 0x3f060 | same; exactly 1440 bytes after sin |
| ~~Ridge FIR taps (13-tap symmetric bandpass)~~ | ~~0x400f0~~ | (superseded by the 0x3ffd0 entry below: this was a straddling misread of the orientation-9/10 kernels, peak 6572) |
| **Descriptor weight kernels (pyramid)** | 0x40130+ | 2× 9-tap + 4× 11-tap symmetric kernels (peaks 118/207/308/392/425); 121 values match the 11×11 descriptor |
| **11-orientation ridge-template kernels** (lengths 5,5,5,7,7,7,9,9,9,11,11 packed i32, normalized sum 32768) | RVA 0x3ffd0 (= file offset 0x3ebd0) | shipped in the driver's `egis0575-matcher.c` (fir_taps/fir_len); orientation 4 is 7 taps (an earlier extraction wrongly padded it to 9 — corrected 2026-09-13) |
| **atan2 LUT (monotonic u16)** | 0x40320 | strictly monotonic 0→358 (128 entries); code applies `720−x` + rounded-division indexing |
| Sensor config table | 0x47794 | contains 88/52/103 = normalized height / sensor height / width |

## 3. Key code sites (located via resynchronized disassembly)

- **0x15515**: `lea rsi,[rip+…]` = 0x40130 → convolution loop `imul`
  image-gradient × weight kernel = **descriptor construction function**
- **0x153bf/0x153e4**: `movzx word [0x40320 + idx*2]` + `720−x`
  = **gradient-orientation computation**

## 4. Replica iteration log (key insight: axial ≠ orientation)

**Filter-bank reinterpretation**: the 11 kernels are not 11 scales but
**ridge matched filters for 11 orientations** (diagonal orientations need
longer effective support → more taps). Correct implementation: oriented
FIR per orientation θ → max response = enhanced image + orientation map
(steerable filter bank). The axis-separable smoothing variant was
falsified (interest points collapsed, genuine/impostor inverted).

| Variant | Genuine votes | Impostor votes | Conclusion |
|---|---|---|---|
| 32-bit + Lowe + median | 0 | — | descriptor too weak |
| 128-bit + Lowe + median | 0-1 | — | Lowe kills true pairs (neighbors too similar) |
| 128-bit, no Lowe + translation-bucket clustering | 5,3,3 | 0,3,5 | first signal |
| 256-bit (16 bins) | 3,2,2 | 3,2,3 | finer bins unstable (SNR limit) |
| + axial FIR smoothing | 0 | 3-4 | falsified (axial ≠ orientation) |
| **+ 11-orientation filter bank** | **9,7,8 / 5 / 7,7,6** | 3-7 | votes still overlap |
| + weighted score Σ(128−h) | mean 156 (min 107) | mean 86 (max 137) | **1.8× separation, still overlapping** |

Methodology takeaways: capstone desync-resync; no Lowe in
weak-descriptor regimes; votes don't discriminate — use weighted scores.
Interest-point geometric repeatability of 52% proved the pipeline healthy.

## 5. The winning recipe (fully traceable, 🏆 early hours of 2026-09-13)

1. 11-orientation ridge matched filters (DLL-extracted taps) → enhanced
   image + orientation map
2. Filter-response maxima as interest points + density NMS (52%
   cross-press repeatability)
3. 512-bit top-tier descriptor: 4×4 regions × 32 orientation bins, DLL
   pyramid weights, main orientation = winning filter orientation
4. Hamming NN (budget 115/512, no Lowe) → translation clustering
   (8-px buckets, mode vote — not iterative RANSAC) → angle-mode voting
5. Windows scoring Σ(128−h) − unmatched-feature penalty (h/2)
6. Dual-frame gallery agreement (defeats position-specific accidental
   alignments)

## 6. Offline validation: perfect separation

**Verdict = single-frame best score ≥244 AND ≥2 gallery frames agreeing (score ≥150)**

| Session | Probe scores / agreeing frames | Verdict |
|---|---|---|
| Genuine A (right × right template) | 254/3, 403/4, 411/4 | ✓ |
| Genuine C (right × right template) | 466/4, 280/3, 374/4 | ✓ |
| Impostor A (left × right template) | 0, 199/3⚠, **438/1**, 0, 0, 239/1 | ✗ (438 killed by the 1-frame agreement rule) |
| Impostor C (left × right template) | 213/1, 0, 147/0, 180/1, 209/2⚠, 189/1 | ✗ |

**FRR 0% (6/6), FAR 0% (0/12).** ⚠ = single low-score accidental
agreement, naturally filtered by the dual condition.

## 7. C port and on-hardware integration acceptance

- **Score-scale difference**: the C engine's descriptor bits differ
  slightly from the Python reference (different interpolation path in the
  oriented filter). **Recalibrated 2026-09-13** (after the orientation-4
  kernel fix, tie-break fix and weight-index clamp; 23 verify-run datasets,
  125 probes, run-level best single-frame score): confident-genuine runs
  bottomed at **349**, confident-impostor runs peaked at **288** (321
  including ambiguous runs) → threshold **335** (gap midpoint), agreement
  **≥2 frames × ≥150** unchanged. (Superseded pre-fix numbers: genuine
  336–470 / impostor 127–280 / threshold 300.)
- **On-hardware integration** (early hours of 2026-09-13, pre-fix engine
  + v0 templates): right index ×3 = 622/404/381, all MATCH (early-exit
  verdict ~2 s); left-index impostor = 285, correctly rejected. These were
  measured with the pre-fix engine and v0 templates (orientation lost on
  reload) and must be re-validated with re-enrollment
- **System level** (full fprintd chain, 00:39; pre-fix engine + old
  threshold 300 — the current threshold is 335): `probes=1
  best_score=1086/300 => MATCH`, coverage 51%
- Not ported from the original Windows engine: the adaptive threshold-660
  scheme — precise semantics completed 2026-10-04 (v3.7.1.1
  `FUN_1800067c0`): effective threshold starts/floors at 735, −80 on
  success, +60 on failure, capped at 990 (1.5×); it is anti-hammering
  hysteresis, not a leniency drift, with a second match-context metric gate
  (85); state persisted in the 'AE' blob (key DAT_180038248). Not portable
  at our current score calibration (genuine margin is 14 points; argument
  in the new doc §3.3); the in-memory template feedback is implemented. See
  [windows-agc-threshold.en.md](windows-agc-threshold.en.md)

## 8. Remaining fidelity gaps (historical list, now optional enhancements)

1. Three descriptor tiers of 29/49/69 bytes + type byte + ID + full
   atan2-LUT orientation (the fixed 512-bit top tier suffices today)
2. Second-chance NN penalty (unmatched features penalized by hamming/2) —
   a simplified version is implemented
3. Per-position ridge-period kernel selection (currently global
   11-orientation max)
4. Full disassembly of 0x15515 = the final answer on bit packing

## 9. Porting blueprint (from decompilation, version-independent algorithm structure)

1.75× resample → flat-field normalization → ridge-period FIR bank →
structure tensor → filter-response maxima interest points → subpixel +
orientation → 11×11 binary orientation histogram → Hamming NN +
median-translation / angle-consistency geometric voting → score =
Σ(128−hamming) (original threshold 660, adaptive). Constraints: ≥11
features per image, ≥5 consistent pairs, Hamming ≤115/512, geometry
8 px/7° (12° in the C port). Current values in `egis0575-matcher.h`.
