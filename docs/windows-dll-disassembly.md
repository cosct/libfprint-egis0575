# Complete Disassembly of `EgisTouchFPEngine0575.dll` Feature Extraction & `MatchMinutia`

This document records the complete instruction-level reverse-engineering of the feature extraction and matching pipeline in `EgisTouchFPEngine0575.dll` (`0x180006250`–`0x180016600`), resolving the open questions listed in [`windows-engine-tables.md`](windows-engine-tables.md) (§8) and [`windows-engine-tables.en.md`](windows-engine-tables.en.md).

---

## 1. Summary of Key Discoveries

1. **The 11 FIR Kernels (`fir_taps` at `.rdata` `0x180043070`) are a $\sigma = 1.0\dots2.0$ Gaussian Blur Lookup Table, NOT an 11-Orientation Filter Bank:**
   - In `0x180014140` (`0x180014211`–`0x180014262`), the DLL computes `idx = config[0] - 10` (`lea eax, [rcx-0xa]`).
   - Because `0x180012b50` initializes `config[0] = 0x10` (`16`), the DLL **only ever selects `fir_taps[6]`** (`{361, 1415, 3757, 6748, 8205, 6749, 3757, 1415, 361}`, 9 taps, sum `32768`) and passes it to `0x180013d60` to perform a **separable 2D Gaussian blur** (horizontal pass followed by vertical pass, `>> 15`).

2. **Keypoints are Local Maxima of the Span-2 Scharr Hessian Determinant (`0x180014530` + `0x180014660`):**
   - `0x18000f6b0` computes span-2 (`x±2`, `y±2`) `[3, 10, 3] / 64` Scharr derivatives (`sar eax, 6`).
   - `0x180014530` calls `0x18000f6b0` three times:
     1. On the Gaussian-blurred image $\to (g_x, g_y)$
     2. On $g_x \to (I_{xx}, I_{xy})$
     3. On $g_y \to (\text{NULL}, I_{yy})$
   - `0x180014660` computes the **Hessian determinant** at every pixel:
     $$\det(H) = (I_{xx} \cdot I_{yy} - I_{xy}^2) \gg 4$$
     and stores a 1-bit ridge/valley polarity class `type = (Ixx < 0)` (`shr eax, 0x1f`, stored at feature offset `+0x0c`).
   - `0x1800147f0` detects strict `3×3` local maxima of $\det(H)$ with border margin `11`, followed by `3×3` quadratic Taylor subpixel interpolation (`0x180014b40`) and spatial NMS (`0x180014c80`).

3. **Keypoint Orientation is a Continuous `0..359°` Gaussian-Weighted Gradient Histogram (`0x180014df0` + `0x180014ec0`):**
   - The `11×11` separable weight tables `w_row` and `w_col` (`0x1800431d0`, peaks `425`/`392`) are used inside `0x180014ec0` (`movsxd rdx, dword ptr [r13 + rbx*4]`, `imul edx, [r12 + rcx*4]`) to weight `(gx, gy)` in an `11×11` window (`[x-5..x+5, y-5..y+5]`) around each keypoint.
   - Each pixel's gradient angle is looked up in `0..359°` via the `atan2` LUT (`0x180014df0`), accumulating weighted `(gx, gy)` into a 360-bin circular histogram.
   - A `36°` sliding window (`config[4]/2 = 18°` half-width) finds the dominant orientation in **`0..359°`**.

4. **The Descriptor (`0x180015200` + `0x180015600`) is a `486`-Bit Multi-Scale (`2×2`, `3×3`, `4×4`) Rotated All-Pairs Ordinal Rank Descriptor:**
   - `0x180015200` rotates a `[-R, +R] × [-R, +R]` (`R = config[0x10] = 9` px) patch around the keypoint by its dominant angle `0..359°` using the 360-entry `sin`/`cos` Q16 tables (`0x180041b60`, `0x180042100`).
   - For each grid scale $N \in \{2, 3, 4\}$ (`config[0x14]=2` to `config[0x18]=4`), it divides the rotated patch into $N \times N$ cells (`4`, `9`, and `16` cells) and averages 3 channels per cell:
     - Channel 0: smoothed intensity $I$
     - Channel 1: rotated gradient $g_x \cos\theta + g_y \sin\theta$
     - Channel 2: rotated gradient $g_x \sin\theta - g_y \cos\theta$
   - `0x180015600` (`setg dl; shl dl, cl; or [rax], dl`) performs an **all-pairs tournament** across all cell pairs $(i, j)$ with $0 \le i < j < N^2$ for each scale and channel:
     $$\text{bit} = \mathbb{I}(\text{cell}[i] > \text{cell}[j])$$
   - Total descriptor length:
     $$\left(\binom{4}{2} + \binom{9}{2} + \binom{16}{2}\right) \times 3 = (6 + 36 + 120) \times 3 = 162 \times 3 = \mathbf{486\text{ bits}} \text{ (61 bytes)}$$
   - Because every descriptor is an all-pairs rank tournament, it always contains ~50% `1`-bits (~243 bits), causing random unrelated features to concentrate tightly around Hamming distance `243` while genuine matches stand out cleanly below `115`.

5. **`MatchMinutia` (`0x18000a610`) Enforces Polarity, Lowe Ratio, Center-Rotated Translation Clustering, and Second-Pass Spatial Verification (`0x18000a150`):**
   - Requires matching Hessian polarity `a->type == b->type` (`0x18000a821`).
   - Filters global 1-NN descriptor matches using Lowe's second-best ratio test (`best_h * 1024 < second_best_h * config[0x40]`) or strict Hamming budget (`0x18000a8bb`).
   - Finds the dominant orientation difference mode(s), rotates gallery coordinates `(x, y)` by `-mode_angle` around the image center `(w/2, h/2)` (`0x180009c40`), and clusters translation `(dx, dy)` (`0x180009760`).
   - Runs a **second-pass spatial neighbor lookup (`0x18000a150` + `0x18000aeb0`)** within radius `6 px` (`d^2 <= 36`) at `(best_dx, best_dy)` over same-polarity features to rescue spatial inliers missed by global 1-NN and penalize/reject alignments with spatial contradictions.

6. **Pre-Calibration Sensor Reset (`SM_PRE_CAL_RESET`):**
   - After `SM_SHUTDOWN` (`71 45 06 ...`) places the sensor into detect/sleep mode at the end of a capture session, running `SM_CAL_PHASE_1..5` directly returns an all-zero calibration block in `SM_CAL_READ_REQ`.
   - Sending `EGIS0575_PRE_RESET_PACKETS` (`97 00 00`) and polling `60 00` (`SM_PRE_CAL_RESET`) *before* `SM_CAL_PHASE_1` wakes the sensor cleanly so `SM_CAL_READ_REQ` succeeds on the first read.
