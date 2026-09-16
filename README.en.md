# libfprint-egis0575 — the EgisTec EH575 (1c7a:0575) Linux fingerprint driver

> [中文版](README.md)

Make the EgisTec EH575 fingerprint sensor (found in the Acer SFX14-41G and
similar machines) genuinely usable on Linux — accuracy at the level of
everyday desktop unlock. The driver source ships in this repository
(the `libfprint/` subtree carrying the full upstream history); the
reverse-engineering research and engineering decisions are documented in
docs/. The end goal is a driver submittable to
[libfprint upstream](https://gitlab.freedesktop.org/libfprint/libfprint).

## Install

- **Arch**: `yay -S libfprint-egis0575` (the AUR package and this repo
  share the name and the source)
- **Debian / Fedora**: [GitHub Releases](https://github.com/cosct/libfprint-egis0575/releases)
  carry `.deb` and `.rpm` artifacts (built automatically per `egis0575-v*` tag)
- **Local development**: `makepkg -f -i` (root PKGBUILD, builds from the
  working tree)
- **⚠ Upgrade note**: v0.2.0 template serialization dropped the orientation
  field that scoring depends on, so prints enrolled with v0.2.0 cannot
  verify on the new driver — after upgrading, run `fprintd-delete` and
  re-enroll

## Status (2026-09-13)

- **Driver works**: press capture architecture + Windows-engine matcher
  port, verified on real hardware (KDE lock screen / fprintd / Bitwarden
  polkit unlock). Matcher validation numbers (small-sample,
  single-machine: offline 6-genuine / 12-impostor FRR/FAR 0%,
  on-hardware impostor separation margin
  47 — post-recalibration (threshold 335 vs peak 288); the pre-fix
  engine's 15 is superseded) in
  [comparison §6/§9](docs/comparison.en.md#6-the-windows-engine-port-the-endgame)
- **v0.2.1 optimizations**: enrollment same-spot rejection (the Windows
  HIGHLY_SIMILARITY port, threshold 650, verified precise on hardware),
  host-side calibration cache surviving close (Windows-shaped), and
  two-tier idle polling (230/500 ms)
- **Unreleased on master**: unlock-latency cuts (120 ms poll cadence for 15 s after
  action start / finger events + press-settle window 400→250 ms),
  verify-time template feedback (the in-memory analogue of the Windows
  'AE' blob: confident-match frames taught back into the gallery for the
  fprintd process lifetime,
  [comparison §6](docs/comparison.en.md#6-the-windows-engine-port-the-endgame)),
  and the v2 template embedding the calibration block (when the
  sensor-side read is invalidated, the template's copy is uploaded for an
  instant recovery; **re-enroll to get a v2 template** — v1 templates
  still verify fine)
- **Todo**: wider-sample threshold tuning, feedback from more machines,
  the upstreamable patch series

## Documentation map

Find the section by question (the bilingual master index lives in
[docs/README.md](docs/README.md)):

| Question | Where |
|---|---|
| What an install/build depends on | this page: [Repository layout](#repository-layout) · [Reproducing the research workflow](#reproducing-the-research-workflow) |
| Wire protocol & command semantics (CET300 set, init sequences, open items) | [protocol §2](docs/protocol.en.md#2-command-and-response-format) · [§4](docs/protocol.en.md#4-initialization-sequences) · [§10](docs/protocol.en.md#10-not-yet-reverse-engineered) |
| Why swipe / NCC / Bozorth3 / SigFM all fail | [comparison §5](docs/comparison.en.md#5-matcher-experiment-history-why-seven-schemes-all-failed-2026-09-12) |
| Where the matcher comes from; coefficient extraction & validation | [windows-engine-tables](docs/windows-engine-tables.en.md) · [comparison §6](docs/comparison.en.md#6-the-windows-engine-port-the-endgame) |
| How Windows does enrollment (origin of the same-spot reject) | [windows-enrollment](docs/windows-enrollment.en.md) |
| How Windows actually drives the sensor (calibration cache, duty cycle) | protocol §4C session order · §7 online behavior ([docs/protocol.en.md](docs/protocol.en.md)) |
| Stability: pitfalls, watchdogs, hang recovery | [protocol §8](docs/protocol.en.md#8-known-pitfalls-all-verified-on-hardware-2026-09-1213) · [comparison §7](docs/comparison.en.md#7-stability-engineering-results-all-hardware-verified) |
| How the enrollment similarity threshold was calibrated | [enroll-sim-calibration.txt](docs/enroll-sim-calibration.txt) · [calibrate-enroll-sim.py](scripts/calibrate-enroll-sim.py) |
| Optimization roadmap & completed items | [optimization-plan.md](docs/optimization-plan.md) (Chinese working doc) |

## Key findings at a glance

1. The EH575 is an **image sensor** (tiny 103×52 frames, host-side
   matching, **no dead columns**) — not match-on-chip
   ([protocol §3](docs/protocol.en.md#3-image-geometry-settled))
2. swipe+Bozorth3 (the topni1 driver) is the root cause of the poor
   accuracy; every classic matching scheme (NCC / Bozorth3 / POC / SigFM
   / orientation field) **fails** to separate same-person cross-finger
   attempts at this sensor's raw SNR
   ([comparison §5](docs/comparison.en.md#5-matcher-experiment-history-why-seven-schemes-all-failed-2026-09-12))
3. This driver = EH577's press capture architecture + topni1's
   calibration init (a hard requirement for imaging on EH575) + the
   **Windows-engine matcher port** (11-orientation ridge matched-filter
   bank, 512-bit descriptors, Hamming/translation-cluster scoring, with
   coefficients extracted from the vendor DLL; small-sample offline
   FRR/FAR 0%, on-hardware integration accepted —
   [comparison §6](docs/comparison.en.md#6-the-windows-engine-port-the-endgame))
4. `01 01 01 → re-run PRE_INIT` is EH575's intended error handling
   ([protocol §4B](docs/protocol.en.md#4-initialization-sequences))
5. The topni1 `FPI_DEVICE_Egis0575` typo recorded earlier was re-checked
   across that fork's whole history and **does not exist**
   ([comparison §8](docs/comparison.en.md#8-upstream-issues-found)); nothing
   to report

## Reproducing the research workflow

```bash
# 0) One-time: clone this repository and build (the driver source is
#    in-tree under libfprint/). (Needs meson>=0.62 + ninja plus dev
#    packages for glib2/libusb/libgusb/pixman/openssl/libgudev; on Arch
#    also gobject-introspection, gtk-doc and glib2-devel.)
git clone https://github.com/cosct/libfprint-egis0575 && cd libfprint-egis0575
meson setup libfprint/builddir libfprint
meson compile -C libfprint/builddir

# 1) One-time: install a temporary udev rule for direct device access
#    (asks for the sudo password)
./scripts/setup-access.sh

# 2) 20 s fingerless probe: verify init stability, background warm-up,
#    and no idle-poll timeouts
./scripts/probe.sh

# 3) Collect a dataset: press as the script instructs (keep the sensor
#    empty for the first 3 s for background warm-up)
./scripts/collect-dataset.sh press-test 60

# 4) Column-activity analysis (settled: all 103 columns active, no dead
#    zone; the script is useful to re-check new machines)
python3 scripts/analyze-columns.py datasets/press-test-*/
```

Python-script dependencies are listed in `requirements.txt` (Python ≥ 3.9
+ numpy; egis_matcher.py's feature extraction and eval_sigfm need
OpenCV; hwpoll needs pyusb). This
repository (docs/scripts/tools) is licensed LGPL-2.1-or-later like the
driver (see `LICENSE`).

### Driver tunables (environment variables)

- `EGIS0575_ACTIVE_WIDTH` — active column count (default 103; settled —
  no dead zone — kept for experiments)
- `EGIS0575_SKIP_CALIBRATION=1` — skip calibration upload and use the
  EH577-style init (known to produce all-zero frames; A/B testing only)
- `EGIS0575_PGM_DEBUG_DIR` / `_LOG` / `_INTERVAL_MS` / `_CONTROL` — PGM
  dataset capture (in capture mode the driver keeps dumping processed
  frames and enroll/verify actions never complete — stateless probing
  only; the `_CONTROL` file can pause/resume capture)
- `EGIS0575_FRAME_DUMP_DIR` — raw 5356-byte frame dumps
- `EGIS0575_LIVE_FRAME_PATH` — live frame written to a single PGM
- `EGIS0575_VERIFY_DUMP_DIR` — dump verify-time probes and gallery
  feature counts (offline matcher analysis)
- `EGIS0575_DISABLE_STRETCH=1` — disable stretch5 contrast enhancement
- `EGIS0575_ENROLL_SIM_THRESHOLD` — enrollment same-spot reject threshold
  (default 650; 0 disables; calibration in
  [enroll-sim-calibration.txt](docs/enroll-sim-calibration.txt))
- `EGIS0575_FINGER_SETTLE_MS` — press-settle window (default 250 ms;
  smaller unlocks faster at the cost of more unstable frames; A/B knob)
- `EGIS0575_VERIFY_FEEDBACK=0` — disable verify-time template feedback
  (default on: confident-match frames are taught into the in-memory
  gallery, effective for the fprintd process lifetime; keep fprintd
  resident to retain it across sessions — write
  `/etc/systemd/system/fprintd.service.d/keepalive.conf` with an
  `[Service]` section containing a clearing `ExecStart=` line followed by
  `ExecStart=/usr/lib/fprintd -t`, then `systemctl daemon-reload`; at the
  cost of continuous low-rate sensor polling, covered by the health
  watchdog)
- `EGIS0575_DEBUG_MAX_FILES` — file cap for PGM/raw-frame dumps (default
  5000, ≈26 MB of raw frames; all debug sinks are written 0600 into 0700
  dirs since they hold biometric data)

## Repository layout

```
docs/     Bilingual document index in docs/README.md (organized by topic)
libfprint/ Driver source (git subtree: full upstream libfprint history +
          the egis0575 driver, LGPL-2.1+); build dirs builddir/ are
          not committed
scripts/  Test and data-collection tooling (probe / capture / analysis /
          calibration)
tools/    Evaluation utilities (egis0575-matcher-test / eval_bz3 —
          two C tools built via make; hwpoll is a Python script)
packaging/ Release recipes: aur/PKGBUILD (canonical AUR copy) ·
          deb/build-deb.sh · rpm/libfprint-egis0575.spec — invoked by
          the release workflow on egis0575-v* tags; artifacts are
          attached to the GitHub Release and the AUR package is updated
          automatically
PKGBUILD  Local development packaging (builds from the working tree;
          the AUR version lives on AUR)
```

Intentionally not distributed here (size or privacy): `refs/` (six
third-party reference implementations — clone them yourself),
`datasets/` (raw fingerprint frames — biometric data), and `acerdrv/`
(Acer's official Windows driver — copyright EgisTec/Acer).
