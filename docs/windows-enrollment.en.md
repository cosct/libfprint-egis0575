# Windows-Side Enrollment Mechanics

> 中文版：[windows-enrollment.md](windows-enrollment.md)
>
> Sources: static analysis of acerdrv v3.7.1.1 (EgisTouchFPEngine0575.dll),
> cross-checked against the 2021-era Ghidra dump in refs/EgisTec-EH575 and
> the on-hardware EngineAdapter/SensorAdapter traces. Every claim carries
> its evidence; items marked "inferred" are decompilation interpretation,
> not instruction-level verification.

## 1. Architectural position: the WBF engine adapter

On Windows the WinBio framework (WinBio.dll) drives three adapters:
Sensor (capture), Engine (feature extraction & matching), Storage (template
database). EgisTouchFPEngine0575.dll exports exactly one symbol,
`WbioQueryEngineInterface`; every other entry point — including the whole
enrollment family — is handed out through the `WINBIO_ENGINE_INTERFACE`
struct:

| WBF entry point | Egis impl (CTouchSensor::) | Role |
|---|---|---|
| EngineAdapterCreateEnrollment | EngineAdapterCreateEnrollment | create enrollment ctx, set target count |
| EngineAdapterAcceptSampleData | AcceptSampleData | per-frame extraction + quality gate (Purpose=4 enroll; 2 verify) |
| EngineAdapterUpdateEnrollment | EngineAdapterUpdateEnrollment | similarity/redundancy verdict, pool admission |
| EngineAdapterGetEnrollmentStatus | (reads ctx status fields) | completeness / more-samples-needed |
| EngineAdapterCommitEnrollment | (serialize + StorageAddRecord) | finalize template into storage |
| EngineAdapterQueryExtendedEnrollmentStatus | — | extended progress query |

The on-hardware trace (EngineAdapter.txt, 2021-era DLL) confirms the verify
call sequence: `PushDataToEngine → AcceptSampleData (Purpose=2, "Extract
Finish, iResult=0, quality") → IdentifyFeatureSet → Verify: result=0,930 →
GetEnrollmentFeature → Update(commit, size=169959→169959)` — the last step
being the already-documented **verification-time template feedback**
(encrypted registry blob 'AE', 169959 bytes).

## 2. Target sample count: three overlay layers

The sample count is not a constant; it is layered configuration:

1. **Per-sensor-model default** (compiled in): a model query (virtual call
   +0x98) selects parameters — model 5 → 10 frames; models 7/8 → 16 frames;
   others → 16 frames with a different policy bit (parameter ids
   0x4bf/0x4bd/0x4cd/0x4b4; inferred to be MIN_ENROLL_COUNT and policy
   toggles).
2. **WinBio registry override**: DWORD `Minimum Fingerprint Samples` under
   `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\WinBio` — accepted only
   in 8..20, clamped back to 16 otherwise, then **−2** (normal mode) or
   **−1** (alternate mode). Field name `m_MinEnrollFingerCount` (printed by
   the exit log). If the value is absent, the layer-1 default stands.
3. **Device-parameter override**: opens the device-specific registry key
   (name from virtual call +0xa8 — the `EgisFP\FPParameters` family seen in
   the Attach log) and enumerates values against the 20 `ENROLL_CTX_*`
   names (SetEnrollConfig; a match injects the value by id).

## 3. Per-frame gating (AcceptSampleData → UpdateEnrollment)

For each press:

1. **Standalone feature extraction**: `Extract Finish, iResult=%d,
   quality=%d`. iResult != 0 → RejectDetail 9 or 7 (WinBio BAD_CAPTURE,
   user retries, does not consume a slot); iResult ∈ {−6,−3} → E_FAIL.
2. **Image-type check** (only for active-console PBA/logon flows):
   `count_image_type_identifier() != 3` → RejectDetail 5 (BAD_CAPTURE).
3. **UpdateEnrollment verdict** (engine core; return code decides fate):
   - `1 = IMAGE_OK`: feature pooled, CurrentEnrollNum+1;
   - `3 = HIGHLY_SIMILARITY`: too similar to a pooled frame (governed by
     `SIMILARITY_THRESHOLD`) — **not pooled but the count still advances**,
     effectively "move your finger and press again";
   - `4 = REDUNDANT_ACCEPT`: redundancy policy (`REDUNDANT_*` family);
   - `2`: special path through a merge/finalize thunk that records the
     merged size.
4. Debug builds can dump `Enroll_Skeleton_%02d` / `Enroll_Orininal_%02d`
   per frame (`EnrollmentCount=[%d], FSize=[%d]`) — the **template is a
   pooled sequence of per-frame features** (structurally the same as this
   driver's 12-frame gallery), not a fused single template.

## 4. Completion and commit

- **Variable-length enrollment**: `MIN_ENROLL_COUNT`/`MAX_ENROLL_COUNT`
  bracket the accepted-frame count; `MAX_ENROLL_TRY` caps total attempts;
  `ENROLL_PROGRESS_THRESHOLD` drives progress. GetEnrollmentStatus reads
  the completion HRESULT + RejectDetail straight from the context.
- **CommitEnrollment**: serializes the enrollment context into the
  template blob → `WbioStorageAddRecord`; capacity is checked first
  (GetRecordCount vs cap; full → `WINBIO_E_DATABASE_FULL`).
- The context is cleared after commit (ClearEnrollment).

## 5. ENROLL_CTX_* tunables (v3.7.1.1, 20 names at .rdata 0x2e6f0+)

| Parameter | Inferred meaning |
|---|---|
| ENROLL_POLICY | master policy switch |
| MAX_ENROLL_ROTATION | max tolerated inter-frame rotation |
| EXTRACT_SKELETON | skeleton extraction mode |
| MIN_MINUTIAE_COUNT | min minutiae per frame (quality gate) |
| MAX_CANDIDATE_COUNT | candidate-frame cap |
| MAX_ENROLL_TRY | total attempt cap |
| MIN_ENROLL_COUNT / MAX_ENROLL_COUNT | accepted-frame bounds (variable length) |
| REDUNDANT_CHECK_START | frame index where redundancy checks begin |
| REDUNDANT_CONTINUOUS_BOUND | consecutive-redundancy bound |
| EXTRACT_THRESHOLD | extraction quality threshold |
| SELECT_COUNT_THRESHOLD | frames selected into the final template |
| GENERALIZATION_THRESHOLD | template generalization/merge threshold |
| ENROLL_PROGRESS_THRESHOLD | progress advance threshold |
| SIMILARITY_THRESHOLD | HIGHLY_SIMILARITY gate |
| DUPLICATE_THRESHOLD | duplicate-frame gate |
| REDUNDANT_INPUT_THRESHOLD / _TRYCOUNT_POLICY / _THRESHOLD / _IMAGE_POLICY | redundancy policy family |

Defaults are set per sensor model in the engine-core constructor
(§2 layer 1); the registry overrides them item by item.

## 6. Comparison with this driver, and portable improvements

| Windows behavior | This driver today | Assessment |
|---|---|---|
| Target 10/16 frames (model default + registry) | Fixed 12 (`EGIS0575_ENROLL_FRAMES`) | same order; no change needed |
| Per-frame extraction quality gate | Stage-2 gate (grain/ridge/minutiae) | present, equivalent |
| **HIGHLY_SIMILARITY: same-spot repeats not pooled** | Ported (v0.2.1): a new frame scoring >= 650 (`egis0575_m_score`) against any pooled frame is rejected — not pooled, stage does not advance — with a 4-consecutive-reject bail-out (`EGIS0575_ENROLL_SIM_THRESHOLD` tunable, 0 disables) | done: on-hardware calibration same-press p10=1889 vs cross-press max 642, 0% misreject (`enroll-sim-calibration.txt`) |
| Variable-length enrollment (MIN/MAX + MAX_TRY) | Fixed 12 stages | libfprint's `nr_enroll_stages` is static at class init; variable length needs framework changes, not upstreamable. 12 stages + rejected frames not advancing (current behavior) approximates it |
| Template = pooled per-frame features | 12-frame feature gallery | isomorphic ✓ |
| Storage capacity check before commit | managed by fprintd | N/A |
| Verification-time template feedback (169959-B 'AE' blob) | Ported as the in-process memory version (master, on by default, `EGIS0575_VERIFY_FEEDBACK` to disable): high-confidence matched frames feed back into the in-memory gallery, valid for the fprintd process lifetime; the encrypted disk-persisted blob is not ported | done (memory version, 2026-09-14); keeping fprintd resident preserves it across sessions, see comparison §6 |
| Registry-tunable 20-parameter surface | driver env vars | research equivalents suffice |

**Ported (shipped in v0.2.1) — enrollment similarity rejection**: before
`on_frame_accepted_enroll` pools a frame's features, the driver scores the
new frame against every already-pooled frame with the existing
`egis0575_m_score`; at similarity >= 650 (the on-hardware calibrated
threshold, tunable via `EGIS0575_ENROLL_SIM_THRESHOLD` — see
`enroll-sim-calibration.txt`) it skips pooling and stage advance and reports
a retry error (`FP_DEVICE_RETRY_GENERAL`) via `fpi_device_enroll_progress`
so the user moves their finger; after
`EGIS0575_ENROLL_SIM_MAX_REJECTS` (4) consecutive rejects it accepts
anyway, keeping enrollment finishable (the Windows MAX_ENROLL_TRY
analogue). Effect: markedly better position coverage of the 12-frame
gallery — the direct equivalent of Windows' HIGHLY_SIMILARITY +
REDUNDANT family — without touching any libfprint framework constraint.

## 7. Evidence index

- Export table has a single symbol (WbioQueryEngineInterface): v3.7.1.1 PE
  export directory.
- Three-layer sample count: 2021 dump `EngineAdapterCreateEnrollment`
  (RegOpenKeyExA "SOFTWARE\\...\\WinBio" + "Minimum Fingerprint Samples",
  8≤v<21 else 16, −1/−2 adjustment; model 5/7/8 branches set 0x4bf/0x4bd
  params; device-key SetEnrollConfig).
- Per-frame gating: dump `CTouchSensor::AcceptSampleData` (iResult→
  RejectDetail 9/7/5, count_image_type_identifier==3, Purpose==4 enroll
  branch, Enroll_Skeleton_%02d dumps) and `EngineAdapterUpdateEnrollment`
  (IMAGE_OK/HIGHLY_SIMILARITY/REDUNDANT_ACCEPT tri-state + CurrentEnrollNum).
- 20 parameter names: v3.7.1.1 .rdata string cluster, RVA 0x2e6f0–0x2e988.
- Verify call sequence and feedback: `logs/EngineAdapter.txt` /
  `SensorAdapter.txt` on-hardware traces.
