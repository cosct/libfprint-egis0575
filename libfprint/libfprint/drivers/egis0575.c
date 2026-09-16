/*
 * Egis Technology Inc. (aka. LighTuning) 0575 driver for libfprint
 * Press-snapshot architecture ported from the EH577 driver
 * (championswimmer/libfprint-eh577, commit b19955e, LGPL-2.1+):
 * Copyright (C) 2021 Animesh Sahu <animeshsahu19@yahoo.com>
 * Copyright (C) 2026 Arnav Gupta <dev@championswimmer.in>
 * EH575 adaptation for the fprintdriver research tree.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * EH575 press-snapshot driver:
 * - calibration init (read → reset → upload; required for imaging on EH575)
 *   with PRE_INIT/POST_INIT kept as the error-retry path
 * - per-claim frame-read budget with release/re-claim + calibration re-upload
 * - warm-background subtraction, per-touch turn state machine
 * - median denoise + stretch5 + NBIS-based Stage-2 quality gate
 * - Windows-engine matcher port (11-orientation ridge matched-filter bank,
 *   512-bit descriptors, Hamming/RANSAC scoring — egis0575-matcher.c);
 *   full-width 103-column frames (EH575 has no dead columns)
 * - fast 120 ms poll cadence for 15 s after action start / finger events, and
 *   verify-time template feedback (taught frames cached per print for the
 *   fprintd process lifetime)
 * - EGIS0575_ACTIVE_WIDTH / EGIS0575_SKIP_CALIBRATION env knobs kept for
 *   A/B experiments
 */

#define FP_COMPONENT "egis0575"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <nbis.h>

#include "egis0575.h"
#include "egis0575-matcher.h"
#include "drivers_api.h"

/*
 * ==================== Basic definitions ====================
 */

#define EGIS0575_ENROLL_FRAMES 12     /* enrollment presses = gallery frames */

/* Enrollment same-spot rejection (port of the Windows engine's
 * HIGHLY_SIMILARITY verdict): a new frame scoring >= threshold against any
 * already-pooled frame is a same-placement repeat — do not pool it, do not
 * advance the stage, and ask the user to move the finger. Calibrated
 * 2026-09-13 on 130 probe frames (scripts/calibrate-enroll-sim.py):
 * same-press pairs p10=1889 vs same-finger cross-press max=642 — near-total
 * separation; 650 sits just above the cross-press maximum (0% misreject).
 * EGIS0575_ENROLL_SIM_THRESHOLD overrides (0 disables). */
#define EGIS0575_ENROLL_SIM_THRESHOLD_DEFAULT 650
#define EGIS0575_ENROLL_SIM_MAX_REJECTS 4      /* bail-out: always accept after this many consecutive rejects (the Windows engine's MAX_ENROLL_TRY analogue, keeps enrollment finishable) */

/* Struct to share data across lifecycle */
struct _FpDeviceEgis0575
{
  FpDevice      parent;

  gboolean      running;
  gint64        action_start_wait_since; /* bounded start wait for the previous loop's SM_DONE drain */
  gboolean      stop;
  gboolean      finger_reported;
  gboolean      capture_armed;
  gboolean      waiting_for_lift; /* set on turn timeout or accepted frame; cleared on lift */

  guint         pgm_debug_counter;
  gint64        pgm_debug_last_capture_time;

  guint8       *background;
  guint         background_warmup_remaining; /* idle frames still to grab as baseline */

  const Packet *pkt_array;
  int           pkt_array_len;
  int           current_index;
  guint         frame_counter;
  guint         frame_reads_this_claim;
  gboolean      has_pre_init_run;

  guint8       *calibration;       /* 5356-byte block; survives close as upload cache (watchdog/broken-check/dispose drop it) */
  guint         cal_read_recoveries; /* bounded reset+re-read attempts after a broken calibration read */
  gint64        last_finger_activity_ms; /* anchor for the deep-idle cadence */
  gboolean      cal_skip;          /* EGIS0575_SKIP_CALIBRATION=1: EH577-style, no cal */
  const Packet *cal_pkt_array;     /* child-SSM packet array being run */
  int           cal_pkt_len;
  int           cal_pkt_index;
  guint         cal_poll_iters;    /* bounded retry counter for status polls */

  gint64        finger_first_detected_time;
  gboolean      turn_open;

  gint64        waiting_for_lift_since; /* phantom-presence watchdog anchor */

  guint         active_width;      /* responsive columns of the 103-col frame */
  guint         padded_img_width;  /* active_width rounded up to a multiple of 4 */

  GPtrArray    *verify_probes;     /* Egis0575MFeatureSet* per collected probe */
  Egis0575MFeatureSet *verify_gallery;  /* unpacked enrolled feature frames */
  guint         verify_gallery_n;
  guint8       *embedded_cal;      /* v2 template's calibration block: fallback when the sensor read is broken */
  gboolean      cal_recovery_adopted; /* adopted embedded cal this action; first structured frame disproves or clears it */

  Egis0575MFeatureSet *enroll_feats;    /* heap: 12 sets exceed the GObject instance limit */
  guint         enroll_stage;
  gint          enroll_sim_threshold;  /* same-spot reject gate; 0 disables */
  guint         enroll_sim_rejects;    /* consecutive same-spot rejects */

  guint         finger_settle_ms;     /* per-touch settle window (EGIS0575_FINGER_SETTLE_MS) */
  gboolean      verify_feedback;      /* EGIS0575_VERIFY_FEEDBACK=0 disables */
  GPtrArray    *feedback_cache;       /* Egis0575FeedbackEntry*, survives close like the calibration cache */

  guint         startup_timeout_retries;
  guint         timeout_recoveries;   /* consecutive timeout restarts; any successful
                                       * transfer callback resets it */
  gboolean      transfer_in_flight;   /* one protocol transfer at a time; watchdog guard */

  gint64        last_full_init_time;  /* sensor-health watchdog anchors */
  gint64        weak_press_window_start;
  guint         weak_press_events;

  guint         close_poll_retries; /* close_poll_cb polls so far this close */
  FpiSsm       *capture_ssm;        /* for the cancel watchdog */
  gboolean      cancel_watchdog_armed;
  GSource      *cancel_watchdog_source; /* pending watchdog timeout, if any */
};

/* State indices appear numeric in fpi-ssm debug logs ("egis0575-capture
 * entering state N"); this table is the legend: 0=CAL_START, 1=CAL_PHASE_1,
 * 2/3=CAL_POLL_2, 4=CAL_PHASE_3, 5/6=CAL_POLL_4, 7=CAL_PHASE_5, 8/9=CAL_READ,
 * 10=CAL_CHECK, 11=PRE_RESET, 12/13=RESET_POLL, 14=POST_RESET, 15=CAL_ENTER,
 * 16=CAL_WRITE, 17=CAL_ACK, 18=INIT, 19=START, 20/21=REQ/RESP, 22=DONE,
 * 23=SHUTDOWN, 24=FINISH. */
enum sm_states {
  SM_CAL_START,
  SM_CAL_PHASE_1,
  SM_CAL_POLL_2_REQ,
  SM_CAL_POLL_2_RESP,
  SM_CAL_PHASE_3,
  SM_CAL_POLL_4_REQ,
  SM_CAL_POLL_4_RESP,
  SM_CAL_PHASE_5,
  SM_CAL_READ_REQ,
  SM_CAL_READ_RESP,
  SM_CAL_CHECK,
  SM_PRE_RESET,
  SM_RESET_POLL_REQ,
  SM_RESET_POLL_RESP,
  SM_POST_RESET,
  SM_CAL_ENTER_REQ,
  SM_CAL_WRITE,
  SM_CAL_ACK,
  SM_INIT,
  SM_START,
  SM_REQ,
  SM_RESP,
  SM_DONE,
  SM_SHUTDOWN,
  SM_FINISH,
  SM_STATES_NUM
};

/* Child state machine that walks a static Packet array (calibration phases). */
enum packet_ssm_states {
  PACKET_SSM_REQ,
  PACKET_SSM_RESP,
  PACKET_SSM_LOOP,
  PACKET_SSM_DONE,
};

G_DECLARE_FINAL_TYPE (FpDeviceEgis0575, fpi_device_egis0575, FPI, DEVICE_EGIS0575, FpDevice);
G_DEFINE_TYPE (FpDeviceEgis0575, fpi_device_egis0575, FP_TYPE_DEVICE);

/* Multi-frame verify: collect up to this many quality-gated frames within one
 * touch turn and match the best one. */
#define EGIS0575_VERIFY_PROBE_FRAMES 6

/* Idle duty cycling: the HW finger-status command (60 01) does not react
 * to presses on this sensor (measured: 1199 polls, constant 0x01), so idle
 * detection stays frame-based — but at a relaxed cadence. Continuous
 * full-rate capture (17ms cycle) cooks the sensor insensitive within
 * minutes; one capture per ~250ms (~7% duty) keeps sessions sustainable
 * with a quarter-second press latency. After IDLE_DEEP_AFTER_MS without
 * finger activity the poll drops to the deep-idle cadence (~3% duty,
 * ≤500 ms press latency), further easing long fprintd sessions (the
 * Windows driver goes fully silent between auth windows, protocol.md §7 —
 * fprintd's finger-status semantics forbid that, so this is the closest
 * portable approximation).
 *
 * While a verify/enroll action is live the cadence rises to
 * ACTIVE_FRAME_DELAY_MS: the finger typically lands within seconds of the
 * prompt, and the desensitization concern targets hour-long idle polling,
 * not a short auth window. The fast tier is time-boxed to
 * ACTIVE_FAST_WINDOW_MS after the action start / the last finger event
 * (last_finger_activity_ms is re-armed by both): a lockscreen left waiting
 * for minutes falls back to the idle tiers instead of sustaining the
 * elevated bus load. */
#define EGIS0575_ACTIVE_FRAME_DELAY_MS 120
#define EGIS0575_ACTIVE_FAST_WINDOW_MS 15000
#define EGIS0575_IDLE_FRAME_DELAY_MS 230
#define EGIS0575_IDLE_FRAME_DELAY_DEEP_MS 500
#define EGIS0575_IDLE_DEEP_AFTER_MS 30000

/* Sensor-health watchdog. Long continuous polling sessions degrade the
 * sensor into a semi-deaf state: real presses show coverage 2-5% instead
 * of 20-80% (raw_finger_pixels drops from the 5356 idle baseline into the
 * 5200s). Two defenses:
 *  - prophylactic: full re-init (incl. the 97 00 00 sensor reset inside
 *    the calibration chain) after this much continuous uptime;
 *  - reactive: this many weak-press frames inside the window means the
 *    sensor hears presses but cannot see them -> recover now. Tuned
 *    2026-09-16: at 20 the detector only caught a held-down finger on a
 *    fully deaf sensor; the observed post-session pattern is intermittent
 *    re-presses reading 1-16% coverage for ~20 s (verify-2 of the
 *    2026-09-16 5/5 run accumulated 15 weak frames and never triggered),
 *    so the bar drops to 8 — still above a couple of genuine light taps. */
#define EGIS0575_IDLE_REINIT_MS 600000
#define EGIS0575_WEAK_PRESS_EVENTS 8
#define EGIS0575_WEAK_PRESS_WINDOW_MS 8000

/* Startup: recycle the interface claim up to this many times if the first
 * pre-init packet times out. If still stuck after that, fail cleanly. */
#define EGIS0575_STARTUP_TIMEOUT_RECOVERY_MAX        2
#define EGIS0575_STARTUP_TIMEOUT_RECOVERY_DELAY_MS 250
#define EGIS0575_STARTUP_SETTLE_DELAY_MS           150

static gboolean sensor_health_watchdog (FpDeviceEgis0575 *self, FpiUsbTransfer *transfer,
                                        FpiSsm *ssm, FpDevice *dev);
static void calculate_finger_heuristics (FpDeviceEgis0575 *self, FpiUsbTransfer *transfer,
                                         int *out_coverage, int *out_intensity);
static gsize count_finger_pixels_raw (FpiUsbTransfer *transfer);
static gboolean recycle_interface_claim (FpDevice *dev, const char *reason, GError **error);
static void cancel_watchdog_cb (FpDevice *dev, gpointer user_data);
static void capture_transfer_submit (FpDeviceEgis0575 *self, FpiUsbTransfer *transfer,
                                     FpDevice *dev, FpiUsbTransferCallback callback);

/* Watchdog check + full recovery path. Called from the idle (no-finger)
 * branch of save_img with the raw frame available. */
static gboolean
sensor_health_watchdog (FpDeviceEgis0575 *self, FpiUsbTransfer *transfer, FpiSsm *ssm, FpDevice *dev)
{
  gint64 now = g_get_monotonic_time ();
  int coverage = 0, intensity = 0;
  gboolean need_reinit = FALSE;

  if (now - self->last_full_init_time > EGIS0575_IDLE_REINIT_MS * 1000)
    {
      fp_warn ("Sensor-health watchdog: >%d s uptime; prophylactic full re-init",
               EGIS0575_IDLE_REINIT_MS / 1000);
      need_reinit = TRUE;
    }

  calculate_finger_heuristics (self, transfer, &coverage, &intensity);
  if (coverage >= 1 && coverage < EGIS0575_PRESENCE_MIN_COVERAGE_PCT &&
      count_finger_pixels_raw (transfer) < EGIS0575_WEAK_PRESS_MAX_RAW_PIXELS)
    {
      if (self->weak_press_window_start == 0 ||
          now - self->weak_press_window_start > EGIS0575_WEAK_PRESS_WINDOW_MS * 1000)
        {
          self->weak_press_window_start = now;
          self->weak_press_events = 0;
        }
      self->weak_press_events++;
      if (self->weak_press_events >= EGIS0575_WEAK_PRESS_EVENTS)
        {
          fp_warn ("Sensor-health watchdog: %u weak-press frames in window; sensor degraded, recovering",
                   self->weak_press_events);
          need_reinit = TRUE;
        }
    }

  if (!need_reinit)
    return FALSE;

  self->weak_press_events = 0;
  self->weak_press_window_start = 0;
  self->last_full_init_time = now;

  /* Full recovery: recycle the claim, drop the cached calibration (forces
   * a fresh read + the in-protocol 97 00 00 sensor reset) and re-arm. */
  {
    g_autoptr(GError) error = NULL;

    if (!recycle_interface_claim (dev, "sensor-health recovery", &error))
      {
        fp_dbg ("Recovery claim recycle failed: %s", error->message);
        fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
        return TRUE;
      }
    g_clear_pointer (&self->calibration, g_free);
    self->frame_reads_this_claim = 0;
    fpi_ssm_jump_to_state_delayed (ssm, SM_CAL_START, EGIS0575_STARTUP_TIMEOUT_RECOVERY_DELAY_MS);
  }
  return TRUE;
}

/* Per-touch timing. After a finger first lands we ignore frames for
 * finger_settle_ms so the press stabilises before evaluation; the Stage-2
 * quality gate rejects unstable frames anyway, so a short settle mainly
 * saves claim budget and CPU. If no valid frame is captured within
 * TURN_TIMEOUT_MS the turn fails and the driver waits for a real lift
 * before arming a new attempt. EGIS0575_FINGER_SETTLE_MS overrides the
 * settle for A/B experiments. */
#define EGIS0575_FINGER_SETTLE_MS_DEFAULT         250
#define EGIS0575_TURN_TIMEOUT_MS                  1400

/* Startup: grab this many valid (non-zero) idle frames as the warm background
 * before arming finger detection. Without a baseline, finger_detected compares
 * against bg=0 and the sensor's hot idle frame reads as a finger, deadlocking
 * detection. The capture workflow guarantees no finger is present at startup. */
#define EGIS0575_BACKGROUND_WARMUP_FRAMES            3

/* Background refresh gate: idle frames have a narrow pixel spread (std < 10,
 * measured max 9.9) while finger frames are strongly structured (std >= 10.1,
 * p50 ~42). Only flat frames may refresh the warm background, so a light real
 * touch never corrupts the baseline. This replaces the EH577 raw-finger-pixel
 * gate which never fires on EH575 (idle frames are full-frame low-intensity),
 * leaving the background frozen until AGC drift makes idle frames read as a
 * permanently present finger. */
#define EGIS0575_BG_UPDATE_MAX_STD                10.0

/* If a "finger" stays detected this long after an accepted frame, it is
 * drift/corrupt-baseline, not a finger: refresh the baseline and re-arm. */
#define EGIS0575_PHANTOM_LIFT_TIMEOUT_MS          4000

/* Bounded retries for the calibration status polls (60 2d / 60 35 / 60 00);
 * topni1 polled unbounded, but a wedged sensor must fail cleanly. */
#define EGIS0575_CAL_POLL_MAX_ITERS                500

/* Bounded recovery for a broken calibration read (uniform-tail block).
 * Observed 2026-09-14/15: after a capture session the read returns all-zero
 * blocks — the firmware invalidates its calibration RAM and regenerates it
 * in the background over seconds-to-minutes (in-chain 97 resets do not
 * speed it up; a USB port reset wedges the chain's polls for minutes, so
 * waiting is the only reliable heal). Recovery therefore: one immediate
 * re-read for transient corruption, adoption of the verify template's
 * embedded calibration block (v2 prints — the Windows host-side cache
 * carried through fprintd storage), then patient 1 Hz re-reads until the
 * firmware rebuild finishes. Past the cap the action fails cleanly. */
#define EGIS0575_CAL_READ_MAX_RECOVERIES           7
#define EGIS0575_CAL_REREAD_DELAY_MS            1000

static const char *
packet_array_name (const Packet *pkt_array)
{
  if (pkt_array == EGIS0575_POST_CALIBRATION_PACKETS)
    return "post-cal";
  if (pkt_array == EGIS0575_POST_INIT_PACKETS)
    return "post-init";
  if (pkt_array == EGIS0575_PRE_INIT_PACKETS)
    return "pre-init";
  if (pkt_array == EGIS0575_CAL_PHASE_1_PACKETS)
    return "cal-1";
  if (pkt_array == EGIS0575_CAL_PHASE_3_PACKETS)
    return "cal-3";
  if (pkt_array == EGIS0575_CAL_PHASE_5_PACKETS)
    return "cal-5";
  if (pkt_array == EGIS0575_PRE_RESET_PACKETS)
    return "pre-reset";
  if (pkt_array == EGIS0575_POST_RESET_PACKETS)
    return "post-reset";
  if (pkt_array == EGIS0575_SHUTDOWN_PACKETS)
    return "shutdown";

  return "unknown";
}

/* Idle poll cadence: fastest while a client action waits for the finger,
 * fast after finger activity, deep after a quiet spell. */
static guint
idle_frame_delay_ms (FpDeviceEgis0575 *self, FpDevice *dev)
{
  FpiDeviceAction action = fpi_device_get_current_action (dev);
  gint64 now_ms = g_get_monotonic_time () / 1000;

  if ((action == FPI_DEVICE_ACTION_VERIFY || action == FPI_DEVICE_ACTION_ENROLL) &&
      self->last_finger_activity_ms != 0 &&
      now_ms - self->last_finger_activity_ms < EGIS0575_ACTIVE_FAST_WINDOW_MS)
    return EGIS0575_ACTIVE_FRAME_DELAY_MS;

  if (self->last_finger_activity_ms != 0 &&
      now_ms - self->last_finger_activity_ms >= EGIS0575_IDLE_DEEP_AFTER_MS)
    return EGIS0575_IDLE_FRAME_DELAY_DEEP_MS;

  return EGIS0575_IDLE_FRAME_DELAY_MS;
}

static void
report_finger_status (FpDeviceEgis0575 *self,
                      gboolean          present,
                      const char       *reason)
{
  if (self->finger_reported != present)
    fp_dbg ("Reporting finger %s (%s)", present ? "present" : "absent", reason);
  else
    fp_dbg ("Finger remains %s (%s)", present ? "present" : "absent", reason);

  /* Finger contact (or the end of one) keeps the fast idle cadence armed;
   * only a genuinely quiet sensor sinks to the deep-idle cadence. */
  if (present || self->finger_reported != present)
    self->last_finger_activity_ms = g_get_monotonic_time () / 1000;

  self->finger_reported = present;
  fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                           present ? FP_FINGER_STATUS_PRESENT : FP_FINGER_STATUS_NONE,
                                           present ? FP_FINGER_STATUS_NONE : FP_FINGER_STATUS_PRESENT);
}

/* Feature-template serialization:
 *   version 1: (y aa(qqyay)) — a version byte plus frames of features
 *              (x, y, orientation index, 64-byte descriptor);
 *   version 2: (y aa(qqyay) ay) — additionally embeds the 5356-byte
 *              calibration block captured at enroll time. The block is the
 *              Windows host-side cache carried through fprintd's print
 *              storage (Windows persists it in the registry and uploads it
 *              every session, protocol.md §4C): when the sensor-side
 *              calibration read is invalidated (post-session all-zero
 *              block, §8 #3) the embedded copy is uploaded instead.
 *
 * The v0.2.0 driver wrote aa(qqay) and silently dropped the orientation
 * index even though scoring depends on it; prints stored in that format
 * cannot match after a reload and are rejected on unpack (re-enroll).
 * v1 prints remain valid but lack the embedded calibration fallback. */
#define EGIS0575_TEMPLATE_VERSION 2

static GVariant *
pack_feature_frames (const Egis0575MFeatureSet *sets, guint n_sets,
                     const guint8 *calibration)
{
  GVariantBuilder outer;

  g_variant_builder_init (&outer, G_VARIANT_TYPE ("aa(qqyay)"));
  for (guint s = 0; s < n_sets; s++)
    {
      g_variant_builder_open (&outer, G_VARIANT_TYPE ("a(qqyay)"));
      for (int i = 0; i < sets[s].n; i++)
        {
          const Egis0575MFeature *f = &sets[s].f[i];

          g_variant_builder_add (&outer, "(qqy@ay)",
                                 f->x, f->y, f->orient,
                                 g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                            f->desc,
                                                            EGIS0575_M_DESC_BYTES,
                                                            1));
        }
      g_variant_builder_close (&outer);
    }

  return g_variant_new ("(y@aa(qqyay)@ay)",
                        (guint8) EGIS0575_TEMPLATE_VERSION,
                        g_variant_builder_end (&outer),
                        g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                   calibration,
                                                   calibration ? EGIS0575_IMGSIZE : 0,
                                                   1));
}

static gboolean
unpack_feature_frames (GVariant             *data,
                       Egis0575MFeatureSet **out_sets,
                       guint                *out_n,
                       guint8              **out_cal)
{
  Egis0575MFeatureSet *sets;
  GVariant *frames;
  guint8 version;
  guint n_frames, fi;

  if (out_cal)
    *out_cal = NULL;

  if (g_variant_is_of_type (data, G_VARIANT_TYPE ("aa(qqay)")))
    {
      fp_warn ("Enrolled print uses the v0 template format (no orientation data); "
               "it cannot match — delete it and re-enroll");
      return FALSE;
    }

  if (g_variant_is_of_type (data, G_VARIANT_TYPE ("(yaa(qqyay))")))
    {
      g_variant_get (data, "(y@aa(qqyay))", &version, &frames);
      if (version != 1)
        {
          fp_warn ("Enrolled print has unsupported template version %u", version);
          g_variant_unref (frames);
          return FALSE;
        }
    }
  else if (g_variant_is_of_type (data, G_VARIANT_TYPE ("(yaa(qqyay)ay)")))
    {
      GVariant *cal;
      gsize cal_len = 0;
      gconstpointer cal_data;

      g_variant_get (data, "(y@aa(qqyay)@ay)", &version, &frames, &cal);
      if (version != 2)
        {
          fp_warn ("Enrolled print has unsupported template version %u", version);
          g_variant_unref (frames);
          g_variant_unref (cal);
          return FALSE;
        }
      cal_data = g_variant_get_fixed_array (cal, &cal_len, 1);
      if (cal_len != 0 && cal_len != EGIS0575_IMGSIZE)
        {
          fp_warn ("Enrolled print embeds a %zu-byte calibration block (expected %d); ignoring it",
                   cal_len, EGIS0575_IMGSIZE);
        }
      else if (out_cal && cal_len == EGIS0575_IMGSIZE)
        {
          *out_cal = g_memdup2 (cal_data, EGIS0575_IMGSIZE);
        }
      g_variant_unref (cal);
    }
  else
    return FALSE;

  n_frames = (guint) g_variant_n_children (frames);
  if (n_frames == 0 || n_frames > 64)
    {
      g_variant_unref (frames);
      return FALSE;
    }

  sets = g_new0 (Egis0575MFeatureSet, n_frames);

  for (fi = 0; fi < n_frames; fi++)
    {
      GVariant *frame = g_variant_get_child_value (frames, fi);
      guint nf = (guint) g_variant_n_children (frame);
      guint k;

      for (k = 0; k < nf && k < EGIS0575_M_MAX_FEATURES; k++)
        {
          GVariant *feat = g_variant_get_child_value (frame, k);
          GVariant *desc;
          Egis0575MFeature *f = &sets[fi].f[k];
          gsize len = 0;
          const void *raw;

          g_variant_get (feat, "(qqy@ay)", &f->x, &f->y, &f->orient, &desc);
          raw = g_variant_get_fixed_array (desc, &len, 1);
          if (len != EGIS0575_M_DESC_BYTES || f->orient >= EGIS0575_M_N_ORIENT)
            {
              g_variant_unref (desc);
              g_variant_unref (feat);
              g_variant_unref (frame);
              g_variant_unref (frames);
              g_free (sets);
              return FALSE;
            }
          memcpy (f->desc, raw, EGIS0575_M_DESC_BYTES);
          g_variant_unref (desc);
          g_variant_unref (feat);
          sets[fi].n++;
        }
      g_variant_unref (frame);
    }
  g_variant_unref (frames);

  *out_sets = sets;
  *out_n = n_frames;
  return TRUE;
}

/* Verify verdict (see https://github.com/cosct/fprintdriver/blob/master/docs/windows-engine-tables.md): a probe matches when its
 * best gallery score clears the threshold AND at least two gallery frames
 * agree above the agree floor. */
static gboolean
probe_matches_gallery (const Egis0575MFeatureSet *probe,
                       const Egis0575MFeatureSet *gallery,
                       guint                     gallery_n,
                       int                      *best_out)
{
  int best = 0;
  guint agree = 0;

  for (guint g = 0; g < gallery_n; g++)
    {
      int s = egis0575_m_score (probe, &gallery[g], NULL);

      if (s > best)
        best = s;
      if (s >= EGIS0575_M_AGREE_SCORE)
        agree++;
    }

  if (best_out)
    *best_out = best;
  return best >= EGIS0575_M_MATCH_THRESHOLD && agree >= EGIS0575_M_AGREE_FRAMES;
}

/* Verify-time template feedback (in-memory analogue of the Windows engine's
 * 'AE' registry blob, which updates the template after successful verifies):
 * a probe that matched with margin joins the gallery, so the template adapts
 * to unlock-time finger placement and slow drift instead of staying frozen at
 * enrollment. fprintd never re-stores prints after enroll, so the adapted
 * gallery lives in this cache keyed by the enrolled template's serialized
 * bytes; like the calibration cache it lasts for the fprintd process
 * lifetime (a resident fprintd, e.g. systemd drop-in `ExecStart=/usr/lib/fprintd -t`,
 * keeps it across unlock sessions).
 *
 * Safety rails: only probes clearing MATCH_THRESHOLD + MARGIN teach (an
 * impostor must first false-accept by 30 points above the calibrated
 * margin-47 threshold before it could pollute anything); same-placement
 * probes (>= the enroll HIGHLY_SIMILARITY gate against any pooled frame)
 * carry no new information and are skipped; the gallery cap bounds the
 * FAR lottery-ticket growth to 4 extra frames. */
#define EGIS0575_VERIFY_FEEDBACK_MIN_MARGIN 30
#define EGIS0575_VERIFY_FEEDBACK_MAX_FRAMES 16  /* 12 enrolled + up to 4 taught */
#define EGIS0575_VERIFY_FEEDBACK_MAX_PRINTS 4   /* cached galleries (one per enrolled finger) */

typedef struct
{
  GBytes              *enrolled_id;  /* serialized enrolled fpi-data: print identity */
  Egis0575MFeatureSet *frames;       /* enrolled frames first, then taught frames */
  guint                n_enrolled;   /* frames[0..n_enrolled) are never evicted */
  guint                n_frames;
  gint64               last_used;    /* LRU anchor for the per-print cache */
} Egis0575FeedbackEntry;

static void
feedback_entry_free (gpointer data)
{
  Egis0575FeedbackEntry *entry = data;

  g_bytes_unref (entry->enrolled_id);
  g_free (entry->frames);
  g_free (entry);
}

static Egis0575FeedbackEntry *
feedback_cache_lookup (FpDeviceEgis0575 *self, GBytes *enrolled_id)
{
  if (!self->feedback_cache)
    return NULL;

  for (guint i = 0; i < self->feedback_cache->len; i++)
    {
      Egis0575FeedbackEntry *entry = g_ptr_array_index (self->feedback_cache, i);

      if (g_bytes_equal (entry->enrolled_id, enrolled_id))
        {
          entry->last_used = g_get_monotonic_time ();
          return entry;
        }
    }
  return NULL;
}

/* Cache a copy of the freshly unpacked enrolled gallery so later verifies of
 * the same print can pick up taught frames. Evicts the least-recently-used
 * print's gallery past EGIS0575_VERIFY_FEEDBACK_MAX_PRINTS. */
static Egis0575FeedbackEntry *
feedback_cache_store (FpDeviceEgis0575 *self, GBytes *enrolled_id,
                      const Egis0575MFeatureSet *gallery, guint gallery_n)
{
  Egis0575FeedbackEntry *entry;

  if (!self->feedback_cache)
    self->feedback_cache = g_ptr_array_new_with_free_func (feedback_entry_free);

  if (self->feedback_cache->len >= EGIS0575_VERIFY_FEEDBACK_MAX_PRINTS)
    {
      guint oldest = 0;

      for (guint i = 1; i < self->feedback_cache->len; i++)
        {
          Egis0575FeedbackEntry *e = g_ptr_array_index (self->feedback_cache, i);

          if (e->last_used < ((Egis0575FeedbackEntry *) g_ptr_array_index (self->feedback_cache, oldest))->last_used)
            oldest = i;
        }
      g_ptr_array_remove_index (self->feedback_cache, oldest);
    }

  entry = g_new0 (Egis0575FeedbackEntry, 1);
  entry->enrolled_id = g_bytes_ref (enrolled_id);
  /* Allocate with headroom for taught frames so feedback_teach can append. */
  entry->frames = g_malloc (MAX (gallery_n, EGIS0575_VERIFY_FEEDBACK_MAX_FRAMES) *
                            sizeof (Egis0575MFeatureSet));
  memcpy (entry->frames, gallery, gallery_n * sizeof (Egis0575MFeatureSet));
  entry->n_enrolled = gallery_n;
  entry->n_frames = gallery_n;
  entry->last_used = g_get_monotonic_time ();
  g_ptr_array_add (self->feedback_cache, entry);
  return entry;
}

/* Fold a confidently matched probe into the cached gallery for this print. */
static void
feedback_teach (FpDeviceEgis0575 *self, GBytes *enrolled_id,
                const Egis0575MFeatureSet *probe, int best_score)
{
  Egis0575FeedbackEntry *entry = feedback_cache_lookup (self, enrolled_id);
  gint dedup_gate = self->enroll_sim_threshold > 0 ?
                    self->enroll_sim_threshold : EGIS0575_ENROLL_SIM_THRESHOLD_DEFAULT;

  if (!entry)
    return;

  /* A hand-crafted enrolled blob can exceed the cap on its own (unpack
   * tolerates up to 64 frames); leave such galleries untouched. */
  if (entry->n_enrolled >= EGIS0575_VERIFY_FEEDBACK_MAX_FRAMES)
    return;

  /* Same-placement repeat of a pooled frame: nothing new to learn. */
  for (guint i = 0; i < entry->n_frames; i++)
    if (egis0575_m_score (probe, &entry->frames[i], NULL) >= dedup_gate)
      {
        fp_dbg ("Verify feedback: probe duplicates gallery frame %u; skipping", i);
        return;
      }

  if (entry->n_frames >= EGIS0575_VERIFY_FEEDBACK_MAX_FRAMES)
    {
      /* FIFO among taught frames; the enrolled frames stay pinned. */
      guint n_taught = entry->n_frames - entry->n_enrolled;

      if (n_taught > 1)
        memmove (&entry->frames[entry->n_enrolled], &entry->frames[entry->n_enrolled + 1],
                 (n_taught - 1) * sizeof (Egis0575MFeatureSet));
      entry->n_frames--;
    }

  entry->frames[entry->n_frames++] = *probe;
  fp_info ("Verify feedback: taught new frame (score %d, gallery now %u frames, %u taught)",
           best_score, entry->n_frames, entry->n_frames - entry->n_enrolled);
}

static void
clear_background (FpDeviceEgis0575 *self)
{
  g_clear_pointer (&self->background, g_free);
}

/*
 * Keep a rolling copy of the most recent *warm* no-finger frame as the background.
 *
 * The sensor's fixed hot/saturated pixels (and fixed-pattern offset) are present in
 * the no-finger frames too, so subtracting the latest one in save_img cancels
 * that fixed-pattern noise without eroding ridge detail. The baseline must be a warm
 * frame that actually carries the hot pixels: a cold all-zero frame has bg=0 at those
 * locations and would fail to subtract them, so only frames that reach this helper
 * (non-zero, no-finger) update it, and they update it every time so the baseline
 * reflects the sensor state right before the finger lands.
 */
static void
update_warm_background (FpDeviceEgis0575 *self, FpiUsbTransfer *transfer)
{
  if (transfer->actual_length != EGIS0575_IMGSIZE)
    return;

  if (!self->background)
    self->background = g_malloc (EGIS0575_IMGSIZE);

  memcpy (self->background, transfer->buffer, EGIS0575_IMGSIZE);
}

static gsize
count_finger_pixels_raw (FpiUsbTransfer *transfer)
{
  gsize count = 0;

  for (gsize i = 0; i < transfer->actual_length; i++)
    {
      guint8 val = transfer->buffer[i];
      if (val > 15 && val < 150)
        count++;
    }

  return count;
}

static gsize
count_nonzero_bytes (FpiUsbTransfer *transfer)
{
  gsize nonzero = 0;

  for (size_t i = 0; i < transfer->actual_length; i++)
    if (transfer->buffer[i] != 0)
      nonzero++;

  return nonzero;
}

/* A frame whose every byte is identical (all-0x00 cold, all-0xFF broken
 * imaging) carries no structure. Healthy idle frames have a narrow but
 * non-zero spread, so uniformity is a clean "imaging is broken" signal. */
static gboolean
frame_is_uniform (FpiUsbTransfer *transfer)
{
  if (transfer->actual_length == 0)
    return TRUE;

  for (size_t i = 1; i < transfer->actual_length; i++)
    if (transfer->buffer[i] != transfer->buffer[0])
      return FALSE;

  return TRUE;
}

/* Population standard deviation of the raw frame: idle frames stay below
 * EGIS0575_BG_UPDATE_MAX_STD, finger frames are far above (see header). */
static double
frame_pixel_std (FpiUsbTransfer *transfer)
{
  double sum = 0.0, sq = 0.0;
  gsize n = transfer->actual_length;

  if (n == 0)
    return 0.0;

  for (gsize i = 0; i < n; i++)
    {
      double v = transfer->buffer[i];
      sum += v;
      sq += v * v;
    }

  return sqrt (sq / n - (sum / n) * (sum / n));
}

/* Debug sinks (PGM/raw dumps, metrics log) hold biometric data and can run
 * for hours on a forgotten session: every sink is written 0600 into 0700
 * dirs and capped at EGIS0575_DEBUG_MAX_FILES files (default 5000, ~26 MB
 * of raw frames) so disk usage is bounded. */
#define EGIS0575_DEBUG_DUMP_MAX_FILES_DEFAULT 5000

static guint
debug_dump_max_files (void)
{
  const gchar *env = g_getenv ("EGIS0575_DEBUG_MAX_FILES");
  guint64 v;

  if (!env || !env[0])
    return EGIS0575_DEBUG_DUMP_MAX_FILES_DEFAULT;

  v = g_ascii_strtoull (env, NULL, 10);
  return (v > 0 && v <= G_MAXUINT) ? (guint) v : EGIS0575_DEBUG_DUMP_MAX_FILES_DEFAULT;
}

/* Biometric sinks (frame dumps, verify images, metrics logs) must never be
 * briefly group/world-readable: g_file_set_contents() creates with
 * 0666 & ~umask (typically 0644) and only a later chmod tightens it. Create
 * with an owner-only mode up front and pin it with fchmod() while the file
 * is still empty, so the permissive window never holds fingerprint data. */
static gboolean
write_file_0600 (const gchar *path,
                 gconstpointer data,
                 gsize         len,
                 GError      **error)
{
  const guchar *buf = data;
  int fd = g_open (path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  gsize off = 0;

  if (fd < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create %s: %s", path, g_strerror (errno));
      return FALSE;
    }

  fchmod (fd, 0600);

  while (off < len)
    {
      ssize_t n = write (fd, buf + off, len - off);

      if (n < 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Failed to write %s: %s", path, g_strerror (errno));
          close (fd);
          return FALSE;
        }
      off += n;
    }

  close (fd);
  return TRUE;
}

/* fopen("a") analogue for the metrics log: the append stream is created
 * owner-only from the very first line. */
static FILE *
append_file_0600 (const gchar *path)
{
  int fd = g_open (path, O_WRONLY | O_CREAT | O_APPEND, 0600);
  FILE *f;

  if (fd < 0)
    return NULL;

  fchmod (fd, 0600);
  f = fdopen (fd, "a");
  if (!f)
    close (fd);
  return f;
}

static void
maybe_write_live_frame_pgm (FpImage *img)
{
  const gchar *path = g_getenv ("EGIS0575_LIVE_FRAME_PATH");
  gchar header[64];
  int header_len;
  gsize data_len;
  g_autofree gchar *buf = NULL;
  g_autoptr(GError) error = NULL;

  if (!path || !path[0])
    return;

  header_len = g_snprintf (header, sizeof (header), "P5 %u %u 255\n", img->width, img->height);
  data_len = (gsize) img->width * img->height;
  buf = g_malloc (header_len + data_len);
  memcpy (buf, header, header_len);
  memcpy (buf + header_len, img->data, data_len);

  if (!write_file_0600 (path, buf, header_len + data_len, &error))
    fp_dbg ("live frame write failed: %s", error->message);
}

static void
dump_frame_if_requested (FpDeviceEgis0575 *self,
                         FpiUsbTransfer    *transfer,
                         gsize              nonzero)
{
  const gchar *dump_dir = g_getenv ("EGIS0575_FRAME_DUMP_DIR");
  g_autofree gchar *path = NULL;
  g_autofree gchar *base = NULL;
  g_autoptr(GError) error = NULL;
  guint cap = debug_dump_max_files ();

  if (!dump_dir || dump_dir[0] == '\0')
    return;

  if (transfer->actual_length != EGIS0575_IMGSIZE)
    return;

  if (self->frame_counter >= cap)
    {
      if (self->frame_counter == cap)
        {
          fp_warn ("EH575 frame dump cap reached (%u files); skipping further frames", cap);
          self->frame_counter++;   /* one-shot latch: warn only once */
        }
      return;
    }

  if (g_mkdir_with_parents (dump_dir, 0700) != 0)
    {
      fp_warn ("Failed to create EH575 frame dump dir: %s", dump_dir);
      return;
    }

  base = g_strdup_printf ("%04u-%s-nonzero-%zu.bin",
                          self->frame_counter++,
                          packet_array_name (self->pkt_array),
                          nonzero);
  path = g_build_filename (dump_dir, base, NULL);

  if (!write_file_0600 (path, transfer->buffer, transfer->actual_length, &error))
    {
      fp_warn ("Failed to dump EH575 frame to %s: %s", path, error->message);
      return;
    }

  fp_dbg ("Dumped EH575 frame to %s", path);
}

static gboolean
write_image_pgm (FpImage     *img,
                 const gchar *path,
                 GError     **error)
{
  gchar header[64];
  int header_len;
  gsize data_len;
  g_autofree gchar *buf = NULL;

  header_len = g_snprintf (header, sizeof (header), "P5 %u %u 255\n", img->width, img->height);
  data_len = (gsize) img->width * img->height;
  buf = g_malloc (header_len + data_len);
  memcpy (buf, header, header_len);
  memcpy (buf + header_len, img->data, data_len);

  return write_file_0600 (path, buf, header_len + data_len, error);
}

static gboolean
pgm_debug_enabled (void)
{
  const gchar *dir = g_getenv ("EGIS0575_PGM_DEBUG_DIR");

  return dir && dir[0];
}

static void dump_verify_image (const gchar *kind, guint index, FpImage *img);
static void close_poll_cb (FpDevice *dev, gpointer user_data);
static void egis0575_finish_close (FpDevice *dev);

static gboolean
pgm_debug_control_active (void)
{
  const gchar *control_path = g_getenv ("EGIS0575_PGM_DEBUG_CONTROL");
  g_autofree gchar *contents = NULL;
  gsize len = 0;

  if (!control_path || !control_path[0])
    return TRUE;

  if (!g_file_get_contents (control_path, &contents, &len, NULL) || len == 0)
    return FALSE;

  return contents[0] == '1' || contents[0] == 'f' || contents[0] == 'F' ||
         g_str_has_prefix (contents, "running");
}

static guint
pgm_debug_interval_ms (void)
{
  const gchar *value = g_getenv ("EGIS0575_PGM_DEBUG_INTERVAL_MS");
  gint64 parsed;

  if (!value || !value[0])
    return 100;

  parsed = g_ascii_strtoll (value, NULL, 10);
  if (parsed < 1)
    return 100;

  return (guint) CLAMP (parsed, 1, 10000);
}

/*
 * ==================== Data processing ====================
 */

static gboolean
valid_data (FpiUsbTransfer *transfer)
{
  return count_nonzero_bytes (transfer) > 0;
}

static void
calculate_finger_heuristics (FpDeviceEgis0575 *self, FpiUsbTransfer *transfer, int *out_coverage, int *out_intensity)
{
  int coverage_pixels = 0;
  long long intensity_sum = 0;

  for (size_t i = 0; i < transfer->actual_length; i++)
    {
      guint8 val = transfer->buffer[i];
      guint8 bg = self->background ? self->background[i] : 0;

      if (val > bg + 2)
        val -= bg;
      else
        val = 0;

      if (val > 15)
        {
          coverage_pixels++;
          intensity_sum += val;
        }
    }

  *out_coverage = (coverage_pixels * 100) / transfer->actual_length;
  *out_intensity = coverage_pixels > 0 ? (intensity_sum / coverage_pixels) : 0;
}

static gboolean
finger_detected (FpDeviceEgis0575 *self, FpiUsbTransfer *transfer)
{
  int coverage = 0, intensity = 0;
  gsize raw_finger_pixels;

  calculate_finger_heuristics (self, transfer, &coverage, &intensity);
  raw_finger_pixels = count_finger_pixels_raw (transfer);

  fp_dbg ("finger_detected: coverage=%d%% intensity=%d raw_finger_pixels=%zu",
          coverage, intensity, raw_finger_pixels);
  return coverage >= EGIS0575_PRESENCE_MIN_COVERAGE_PCT &&
         intensity >= EGIS0575_PRESENCE_MIN_INTENSITY &&
         raw_finger_pixels >= EGIS0575_PRESENCE_MIN_RAW_FINGER_PIXELS;
}

/* Normalize the processed image into the polarity NBIS expects before applying
 * the Stage-2 quality gate.  The snapshot images are marked COLORS_INVERTED,
 * so the gate runs on the final resized output geometry that NBIS will
 * actually see. */
static void
normalize_snapshot_image_for_stage2 (FpImage *img)
{
  if (!(img->flags & FPI_IMAGE_COLORS_INVERTED))
    return;

  for (gsize i = 0; i < (gsize) img->width * img->height; i++)
    img->data[i] = 0xff - img->data[i];

  img->flags &= ~FPI_IMAGE_COLORS_INVERTED;
}

static guint
histogram_percentile_value (const guint histogram[256],
                            guint       total,
                            guint       pct)
{
  guint target;
  guint cumulative = 0;

  if (total == 0)
    return 0;

  target = (guint) (((guint64) (total - 1) * pct) / 100);

  for (guint i = 0; i < 256; i++)
    {
      cumulative += histogram[i];
      if (cumulative > target)
        return i;
    }

  return 255;
}

/* Offline testing (EH577) found the ImageMagick-like "stretch5" transform to be
 * the best visibility/minutiae tradeoff so far: map the 5th..99th percentile
 * range of the final normalized snapshot into 20..245. This improves ridge
 * contrast without the heavier local-normalization variants that risk creating
 * synthetic minutiae. The submitted image is intentionally the enhanced image so
 * libfprint/NBIS and saved capture PGMs see the same pixels. */
static void
enhance_snapshot_image_stretch5 (FpImage *img,
                                 guint   *out_p5,
                                 guint   *out_p99)
{
  guint histogram[256] = { 0 };
  guint total = img->width * img->height;
  guint lo;
  guint hi;
  guint in_range;
  guint out_range = EGIS0575_ENHANCE_STRETCH_OUT_HI - EGIS0575_ENHANCE_STRETCH_OUT_LO;

  if (total == 0)
    return;

  for (gsize i = 0; i < (gsize) total; i++)
    histogram[img->data[i]]++;

  lo = histogram_percentile_value (histogram, total, EGIS0575_ENHANCE_STRETCH_LO_PCT);
  hi = histogram_percentile_value (histogram, total, EGIS0575_ENHANCE_STRETCH_HI_PCT);

  if (out_p5)
    *out_p5 = lo;
  if (out_p99)
    *out_p99 = hi;

  if (hi <= lo)
    {
      fp_dbg ("Skipping stretch5 enhancement due to flat histogram (lo=%u hi=%u)", lo, hi);
      return;
    }

  if (g_strcmp0 (g_getenv ("EGIS0575_DISABLE_STRETCH"), "1") == 0)
    {
      fp_dbg ("Skipping stretch5 enhancement because EGIS0575_DISABLE_STRETCH=1");
      return;
    }

  in_range = hi - lo;
  for (gsize i = 0; i < (gsize) total; i++)
    {
      gint v = img->data[i];
      gint stretched;

      if (v <= (gint) lo)
        stretched = EGIS0575_ENHANCE_STRETCH_OUT_LO;
      else if (v >= (gint) hi)
        stretched = EGIS0575_ENHANCE_STRETCH_OUT_HI;
      else
        stretched = EGIS0575_ENHANCE_STRETCH_OUT_LO +
                    (((v - (gint) lo) * (gint) out_range + (gint) in_range / 2) /
                     (gint) in_range);

      img->data[i] = (guint8) CLAMP (stretched, 0, 255);
    }

  fp_dbg ("Applied stretch5 enhancement: p%u=%u p%u=%u -> %u..%u",
          EGIS0575_ENHANCE_STRETCH_LO_PCT,
          lo,
          EGIS0575_ENHANCE_STRETCH_HI_PCT,
          hi,
          EGIS0575_ENHANCE_STRETCH_OUT_LO,
          EGIS0575_ENHANCE_STRETCH_OUT_HI);
}

static guint8
median9 (const guint8 *values)
{
  guint8 sorted[9];

  memcpy (sorted, values, sizeof (sorted));

  for (guint i = 1; i < G_N_ELEMENTS (sorted); i++)
    {
      guint8 v = sorted[i];
      gint j = (gint) i - 1;

      while (j >= 0 && sorted[j] > v)
        {
          sorted[j + 1] = sorted[j];
          j--;
        }

      sorted[j + 1] = v;
    }

  return sorted[4];
}

/* In-place 3x3 median denoise. The press snapshot carries high-frequency
 * speckle that stretch5 then amplifies (a narrow p5..p99 range means a large
 * stretch gain, turning a few grey levels of sensor wobble into >25-level jumps
 * that the grain metric counts as noise). A median filter removes that speckle
 * while preserving ridge edges, so it runs *before* the stretch. Interior
 * pixels only; the 1px border is left untouched (the grain metric ignores it
 * too). Operates on a snapshot copy so each output reads only original pixels. */
static void
denoise_snapshot_median3x3 (FpImage *img)
{
  guint w = img->width;
  guint h = img->height;
  g_autofree guint8 *src = NULL;
  guint8 window[9];

  if (w < 3 || h < 3)
    return;

  src = g_memdup2 (img->data, (gsize) w * h);

  for (guint y = 1; y + 1 < h; y++)
    for (guint x = 1; x + 1 < w; x++)
      {
        guint idx = 0;

        for (gint dy = -1; dy <= 1; dy++)
          for (gint dx = -1; dx <= 1; dx++)
            window[idx++] = src[(y + dy) * w + (x + dx)];

        img->data[y * w + x] = median9 (window);
      }
}

static guint
stage2_grain_pct_x1000 (FpImage *img)
{
  guint64 noisy_pixels = 0;
  guint64 interior_pixels = 0;
  guint8 window[9];

  if (img->width < 3 || img->height < 3)
    return G_MAXUINT;

  for (guint y = 1; y + 1 < img->height; y++)
    {
      for (guint x = 1; x + 1 < img->width; x++)
        {
          guint idx = 0;
          guint8 med;

          for (gint dy = -1; dy <= 1; dy++)
            for (gint dx = -1; dx <= 1; dx++)
              window[idx++] = img->data[(y + dy) * img->width + (x + dx)];

          med = median9 (window);
          if (ABS ((gint) img->data[y * img->width + x] - (gint) med) >
              EGIS0575_STAGE2_GRAIN_DIFF_THRESHOLD)
            noisy_pixels++;

          interior_pixels++;
        }
    }

  if (interior_pixels == 0)
    return G_MAXUINT;

  return (guint) ((noisy_pixels * 100000ULL) / interior_pixels);
}

static guint
stage2_ridge_pixels (FpImage *img)
{
  guint count = 0;

  for (gsize i = 0; i < (gsize) img->width * img->height; i++)
    if (img->data[i] < EGIS0575_STAGE2_RIDGE_PIXEL_THRESHOLD)
      count++;

  return count;
}

static gboolean
stage2_minutiae_count (FpImage *img, guint *out_minutiae)
{
  MINUTIAE *minutiae = NULL;
  g_autofree int *quality_map = NULL;
  g_autofree int *direction_map = NULL;
  g_autofree int *low_contrast_map = NULL;
  g_autofree int *low_flow_map = NULL;
  g_autofree int *high_curve_map = NULL;
  g_autofree unsigned char *binarized = NULL;
  g_autofree LFSPARMS *lfsparms = NULL;
  int map_w = 0, map_h = 0;
  int bw = 0, bh = 0, bd = 0;
  int r;

  lfsparms = g_memdup2 (&g_lfsparms_V2, sizeof (LFSPARMS));
  lfsparms->remove_perimeter_pts = (img->flags & FPI_IMAGE_PARTIAL) ? TRUE : FALSE;

  r = get_minutiae (&minutiae,
                    &quality_map,
                    &direction_map,
                    &low_contrast_map,
                    &low_flow_map,
                    &high_curve_map,
                    &map_w,
                    &map_h,
                    &binarized,
                    &bw,
                    &bh,
                    &bd,
                    img->data,
                    img->width,
                    img->height,
                    8,
                    img->ppmm,
                    lfsparms);
  if (r)
    {
      fp_warn ("Stage-2 minutiae scan failed, code %d", r);
      if (minutiae)
        free_minutiae (minutiae);
      return FALSE;
    }

  *out_minutiae = minutiae ? minutiae->num : 0;
  if (minutiae)
    free_minutiae (minutiae);

  return TRUE;
}

static gboolean
stage2_snapshot_quality_ok (FpDeviceEgis0575 *self,
                            FpImage          *img,
                            guint            *out_grain_pct_x1000,
                            guint            *out_ridge_pixels,
                            guint            *out_minutiae,
                            guint            *out_stretch_p5,
                            guint            *out_stretch_p99)
{
  guint grain_pct_x1000;
  guint ridge_pixels;
  guint minutiae = 0;
  guint stretch_p5 = 0;
  guint stretch_p99 = 0;

  normalize_snapshot_image_for_stage2 (img);
  denoise_snapshot_median3x3 (img);
  enhance_snapshot_image_stretch5 (img, &stretch_p5, &stretch_p99);

  grain_pct_x1000 = stage2_grain_pct_x1000 (img);
  ridge_pixels = stage2_ridge_pixels (img);

  if (!stage2_minutiae_count (img, &minutiae))
    {
      if (out_grain_pct_x1000)
        *out_grain_pct_x1000 = grain_pct_x1000;
      if (out_ridge_pixels)
        *out_ridge_pixels = ridge_pixels;
      if (out_minutiae)
        *out_minutiae = 0;
      if (out_stretch_p5)
        *out_stretch_p5 = stretch_p5;
      if (out_stretch_p99)
        *out_stretch_p99 = stretch_p99;
      return FALSE;
    }

  if (out_grain_pct_x1000)
    *out_grain_pct_x1000 = grain_pct_x1000;
  if (out_ridge_pixels)
    *out_ridge_pixels = ridge_pixels;
  if (out_minutiae)
    *out_minutiae = minutiae;
  if (out_stretch_p5)
    *out_stretch_p5 = stretch_p5;
  if (out_stretch_p99)
    *out_stretch_p99 = stretch_p99;

  return grain_pct_x1000 < EGIS0575_STAGE2_GRAIN_PCT_X1000 &&
         minutiae > EGIS0575_STAGE2_MIN_MINUTIAE &&
         minutiae < EGIS0575_STAGE2_MAX_MINUTIAE &&
         ridge_pixels > EGIS0575_STAGE2_MIN_RIDGE_PIXELS;
}

static FpImage *
create_processed_snapshot (FpDeviceEgis0575 *self,
                           FpiUsbTransfer   *transfer)
{
  g_autoptr(FpImage) img = fp_image_new (self->padded_img_width, EGIS0575_SENSOR_STRIDE_Y);

  img->width = self->padded_img_width;
  img->height = EGIS0575_SENSOR_STRIDE_Y;
  img->flags = FPI_IMAGE_COLORS_INVERTED;

  for (guint src_y = 0; src_y < EGIS0575_SENSOR_STRIDE_Y; src_y++)
    for (guint src_x = 0; src_x < self->active_width; src_x++)
      {
        guint8 val = transfer->buffer[src_y * EGIS0575_SENSOR_STRIDE_X + src_x];
        guint8 bg = self->background ? self->background[src_y * EGIS0575_SENSOR_STRIDE_X + src_x] : 0;

        img->data[src_y * self->padded_img_width + src_x] = (val > bg + 2) ? val - bg : 0;
      }

  return fpi_image_resize (img, EGIS0575_RESIZE, EGIS0575_RESIZE);
}

static void
image_basic_stats (FpImage *img,
                   guint   *out_min,
                   guint   *out_max,
                   guint   *out_mean)
{
  gsize n = (gsize) img->width * img->height;
  guint min = 255;
  guint max = 0;
  guint64 sum = 0;

  for (gsize i = 0; i < n; i++)
    {
      guint v = img->data[i];
      min = MIN (min, v);
      max = MAX (max, v);
      sum += v;
    }

  if (out_min)
    *out_min = min;
  if (out_max)
    *out_max = max;
  if (out_mean)
    *out_mean = n ? (guint) (sum / n) : 0;
}

static void
pgm_debug_maybe_capture (FpDeviceEgis0575 *self,
                         FpiUsbTransfer   *transfer,
                         gboolean          has_valid_data)
{
  const gchar *dir = g_getenv ("EGIS0575_PGM_DEBUG_DIR");
  const gchar *log_path = g_getenv ("EGIS0575_PGM_DEBUG_LOG");
  gint64 now;
  guint interval_ms;
  int coverage = 0;
  int intensity = 0;
  guint raw_nonzero;
  guint raw_finger_pixels;
  gboolean present;
  g_autoptr(FpImage) img = NULL;
  guint grain_pct_x1000 = 0;
  guint ridge_pixels = 0;
  guint minutiae = 0;
  guint stretch_p5 = 0;
  guint stretch_p99 = 0;
  guint pixel_min = 0;
  guint pixel_max = 0;
  guint pixel_mean = 0;
  gboolean quality_ok;
  guint seq;
  gint64 t_ms;
  g_autofree gchar *base = NULL;
  g_autofree gchar *path = NULL;
  g_autoptr(GError) error = NULL;
  FILE *logf;
  gboolean need_header = FALSE;

  if (!pgm_debug_enabled () || !pgm_debug_control_active ())
    return;

  now = g_get_monotonic_time ();
  interval_ms = pgm_debug_interval_ms ();
  if (self->pgm_debug_last_capture_time != 0 &&
      now - self->pgm_debug_last_capture_time < (gint64) interval_ms * 1000)
    return;

  if (transfer->actual_length != EGIS0575_IMGSIZE)
    return;

  if (self->pgm_debug_counter >= debug_dump_max_files ())
    {
      if (self->pgm_debug_counter == debug_dump_max_files ())
        {
          fp_warn ("PGM debug cap reached (%u files in %s); skipping further frames",
                   debug_dump_max_files (), dir);
          self->pgm_debug_counter++;   /* one-shot latch: warn only once */
        }
      return;
    }

  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      fp_warn ("PGM debug: failed to create %s", dir);
      return;
    }

  calculate_finger_heuristics (self, transfer, &coverage, &intensity);
  raw_nonzero = (guint) count_nonzero_bytes (transfer);
  raw_finger_pixels = (guint) count_finger_pixels_raw (transfer);
  present = has_valid_data &&
            coverage >= EGIS0575_PRESENCE_MIN_COVERAGE_PCT &&
            intensity >= EGIS0575_PRESENCE_MIN_INTENSITY &&
            raw_finger_pixels >= EGIS0575_PRESENCE_MIN_RAW_FINGER_PIXELS;

  img = create_processed_snapshot (self, transfer);
  quality_ok = stage2_snapshot_quality_ok (self, img,
                                           &grain_pct_x1000,
                                           &ridge_pixels,
                                           &minutiae,
                                           &stretch_p5,
                                           &stretch_p99);
  image_basic_stats (img, &pixel_min, &pixel_max, &pixel_mean);

  self->pgm_debug_last_capture_time = now;
  seq = ++self->pgm_debug_counter;
  t_ms = now / 1000;
  base = g_strdup_printf ("frame-%06u-t%lld.pgm", seq, (long long) t_ms);
  path = g_build_filename (dir, base, NULL);

  if (!write_image_pgm (img, path, &error))
    {
      fp_warn ("PGM debug: failed to write %s: %s", path, error->message);
      return;
    }

  if (!log_path || !log_path[0])
    return;

  need_header = !g_file_test (log_path, G_FILE_TEST_EXISTS);
  logf = append_file_0600 (log_path);   /* per-frame biometric metrics */
  if (!logf)
    {
      fp_warn ("PGM debug: failed to open metrics log %s", log_path);
      return;
    }

  if (need_header)
    fprintf (logf, "seq,pgm,t_ms,raw_nonzero,raw_finger_pixels,presence,coverage_pct,intensity,grain_pct_x1000,grain_pct,ridge_pixels,minutiae,stretch_p5,stretch_p99,pixel_min,pixel_max,pixel_mean,quality_ok\n");

  fprintf (logf,
           "%u,%s,%lld,%u,%u,%d,%d,%d,%u,%u.%03u,%u,%u,%u,%u,%u,%u,%u,%d\n",
           seq,
           base,
           (long long) t_ms,
           raw_nonzero,
           raw_finger_pixels,
           present ? 1 : 0,
           coverage,
           intensity,
           grain_pct_x1000,
           grain_pct_x1000 / 1000,
           grain_pct_x1000 % 1000,
           ridge_pixels,
           minutiae,
           stretch_p5,
           stretch_p99,
           pixel_min,
           pixel_max,
           pixel_mean,
           quality_ok ? 1 : 0);
  fclose (logf);
}

/* Release + re-claim the interface to unwedge the bulk endpoints. Called
 * from transfer-completion callbacks (timeout recovery, sensor-health
 * watchdog); that is safe only because the driver keeps exactly one
 * protocol transfer in flight at a time (capture_transfer_submit +
 * transfer_in_flight): when a completion callback runs, no other URB is
 * outstanding, so the interface is never released from under in-flight
 * traffic — the condition that wedges the firmware (see dev_close). */
static gboolean
recycle_interface_claim (FpDevice *dev,
                         const char *reason,
                         GError **error)
{
  GUsbDevice *usb_dev = fpi_device_get_usb_device (dev);

  fp_dbg ("Releasing interface %d before fresh claim (%s)", EGIS0575_INTERFACE, reason);
  if (!g_usb_device_release_interface (usb_dev,
                                       EGIS0575_INTERFACE,
                                       0,
                                       error))
    return FALSE;

  fp_dbg ("Re-claiming interface %d (%s)", EGIS0575_INTERFACE, reason);
  if (!g_usb_device_claim_interface (usb_dev,
                                     EGIS0575_INTERFACE,
                                     0,
                                     error))
    return FALSE;

  return TRUE;
}

/* The sensor family appears to budget only ~8 large 64 14 ec reads per
 * interface claim.  Re-running REPEAT as a "flush" spends the same budget and
 * wedges the next stage.  Instead, recycle the claim and restart from the
 * beginning of the packet sequence so every snapshot is captured from a fresh
 * transport session. */
static void
restart_capture_cycle (FpDeviceEgis0575 *self,
                       FpiSsm           *ssm,
                       FpDevice         *dev,
                       const char       *reason,
                       guint             delay)
{
  g_autoptr(GError) error = NULL;

  if (!recycle_interface_claim (dev, reason, &error))
    {
      fp_dbg ("Failed to recycle EH575 claim: %s", error->message);
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }

  self->frame_reads_this_claim = 0;
  if (self->cal_skip)
    {
      self->has_pre_init_run = FALSE;
      fp_dbg ("Restarting capture cycle from fresh claim in %u ms (%s)", delay, reason);
      fpi_ssm_jump_to_state_delayed (ssm, SM_INIT, delay);
    }
  else
    {
      /* Sensor RAM may not survive the claim recycle: re-upload the cached
       * calibration block (POST_RESET -> enter -> write -> ack) before the
       * next polling cycle. */
      self->has_pre_init_run = TRUE;
      fp_dbg ("Restarting from re-arm (calibration re-upload) in %u ms (%s)", delay, reason);
      fpi_ssm_jump_to_state_delayed (ssm, SM_POST_RESET, delay);
    }
}

static gboolean
claim_needs_recycle (FpDeviceEgis0575 *self)
{
  return self->frame_reads_this_claim >= EGIS0575_MAX_FRAMES_PER_CLAIM;
}

static void
restart_for_next_poll (FpDeviceEgis0575 *self,
                       FpiSsm           *ssm,
                       FpDevice         *dev,
                       const char       *reason)
{
  if (claim_needs_recycle (self))
    {
      fp_dbg ("Frame-read budget reached on this claim (%u/%u), recycling before next poll",
              self->frame_reads_this_claim,
              EGIS0575_MAX_FRAMES_PER_CLAIM);
      restart_capture_cycle (self, ssm, dev, reason, 0);
      return;
    }

  fp_dbg ("Retrying next capture immediately without recycling claim (%s)", reason);
  fpi_ssm_jump_to_state (ssm, SM_INIT);
}

/* Multi-frame verify: match every collected probe against the stored
 * feature gallery; verdict when the probe target is reached, the finger is
 * lifted, or the turn times out. */
static void
finalize_verify (FpDeviceEgis0575 *self, FpiSsm *ssm, FpDevice *dev)
{
  FpPrint *verify_print = NULL;
  g_autoptr(GVariant) stored = NULL;
  gboolean match = FALSE;
  int best_overall = 0;
  const Egis0575MFeatureSet *best_probe = NULL;

  fpi_device_get_verify_data (dev, &verify_print);
  g_object_get (verify_print, "fpi-data", &stored, NULL);

  if (!stored || !self->verify_gallery)
    {
      g_ptr_array_set_size (self->verify_probes, 0);
      self->stop = TRUE;
      fpi_ssm_jump_to_state (ssm, SM_DONE);
      fpi_device_verify_complete (dev,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                            "no feature gallery in verify print"));
      return;
    }

  for (guint p = 0; p < self->verify_probes->len; p++)
    {
      const Egis0575MFeatureSet *probe = g_ptr_array_index (self->verify_probes, p);
      int best = 0;

      if (probe_matches_gallery (probe, self->verify_gallery,
                                 self->verify_gallery_n, &best))
        match = TRUE;
      if (best > best_overall)
        {
          best_overall = best;
          best_probe = probe;
        }
    }

  fp_info ("Verify (multi-frame): probes=%u best_score=%d/%d => %s",
           self->verify_probes->len, best_overall, EGIS0575_M_MATCH_THRESHOLD,
           match ? "MATCH" : "NO-MATCH");

  /* Template feedback: only a confident match may teach (see the rails on
   * feedback_teach). Runs before verify_probes is truncated. */
  if (match && self->verify_feedback && best_probe &&
      best_overall >= EGIS0575_M_MATCH_THRESHOLD + EGIS0575_VERIFY_FEEDBACK_MIN_MARGIN)
    {
      g_autoptr(GBytes) enrolled_id = g_variant_get_data_as_bytes (stored);

      feedback_teach (self, enrolled_id, best_probe, best_overall);
    }

  g_ptr_array_set_size (self->verify_probes, 0);

  self->stop = TRUE;
  fpi_ssm_jump_to_state (ssm, SM_DONE);
  /* NULL print: our probe template never equals the enrolled one and a
   * non-NULL print makes fprintd warn "scanned print that is not matching". */
  fpi_device_verify_report (dev,
                            match ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                            NULL,
                            NULL);
  fpi_device_verify_complete (dev, NULL);
}

static void
on_frame_accepted_enroll (FpDevice *dev,
                          FpImage  *img)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  FpPrint *enroll_print = NULL;
  Egis0575MFeatureSet *fs = &self->enroll_feats[self->enroll_stage];

  fpi_device_get_enroll_data (dev, &enroll_print);

  egis0575_m_extract (img->data, img->width, img->height, fs);
  g_object_unref (img);

  /* Same-spot repeat check (Windows HIGHLY_SIMILARITY port). The slot is
   * overwritten by the next attempt anyway, so extracting in place and not
   * advancing the stage rejects the frame without any copying. */
  if (self->enroll_sim_threshold > 0 && self->enroll_stage > 0 &&
      self->enroll_sim_rejects < EGIS0575_ENROLL_SIM_MAX_REJECTS)
    {
      int best_sim = 0;

      for (guint i = 0; i < self->enroll_stage; i++)
        {
          int sim = egis0575_m_score (fs, &self->enroll_feats[i], NULL);

          if (sim > best_sim)
            best_sim = sim;
        }

      if (best_sim >= self->enroll_sim_threshold)
        {
          self->enroll_sim_rejects++;
          fp_info ("Enroll frame rejected as same-spot repeat (sim=%d >= %d, "
                   "reject %u/%u); asking for a new position",
                   best_sim, self->enroll_sim_threshold,
                   self->enroll_sim_rejects, EGIS0575_ENROLL_SIM_MAX_REJECTS);
          /* No FP_DEVICE_RETRY_* code means "move to a different spot";
           * GENERAL ("poor scan quality / general scanning problem") is the
           * honest fit — CENTER_FINGER would tell the user to center a
           * finger that is already well placed. */
          fpi_device_enroll_progress (dev, self->enroll_stage, enroll_print,
                                      fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));

          self->capture_armed = FALSE;
          self->turn_open = FALSE;
          self->waiting_for_lift = TRUE;
          return;
        }
    }

  self->enroll_sim_rejects = 0;
  self->enroll_stage++;

  /* The enrollment template starts as FPI_PRINT_UNDEFINED and is reused across
   * progress callbacks. Only set the final print type once, right before
   * completion, otherwise repeated fpi_print_set_type() calls trip the internal
   * assertion that the type must still be undefined. */
  fpi_device_enroll_progress (dev, self->enroll_stage, enroll_print, NULL);

  self->capture_armed = FALSE;
  self->turn_open = FALSE;
  self->waiting_for_lift = TRUE;

  fp_info ("Enroll stage %u/%u captured (%d features); waiting for lift",
           self->enroll_stage, EGIS0575_ENROLL_FRAMES, fs->n);

  if (self->enroll_stage < EGIS0575_ENROLL_FRAMES)
    return;

  GVariant *feats = pack_feature_frames (self->enroll_feats, EGIS0575_ENROLL_FRAMES,
                                         self->calibration);
  fpi_print_set_type (enroll_print, FPI_PRINT_RAW);
  g_object_set (enroll_print, "fpi-data", feats, NULL);

  self->stop = TRUE;
  fpi_device_enroll_complete (dev, g_object_ref (enroll_print), NULL);
}

/* Single-shot like verify: hand the stage-2-qualifying image straight back to
 * fp_device_capture_finish(). This is the exact resized snapshot the engine matcher
 * sees, so PGM debug tooling stores the same pixels the matcher would.
 * Setting stop=TRUE lets the SSM wind down to SM_DONE after we complete; without
 * it the poll loop would keep running past the completed action. */
static void
on_frame_accepted_capture (FpDevice *dev,
                           FpImage  *img)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  self->stop = TRUE;
  fpi_device_capture_complete (dev, img, NULL);
}

static void
on_frame_accepted (FpDevice *dev,
                   FpImage  *img)
{
  switch (fpi_device_get_current_action (dev))
    {
    case FPI_DEVICE_ACTION_ENROLL:
      on_frame_accepted_enroll (dev, img);
      break;

    case FPI_DEVICE_ACTION_VERIFY:
      /* handled by the multi-frame probe path in save_img */
      g_object_unref (img);
      break;

    case FPI_DEVICE_ACTION_CAPTURE:
      on_frame_accepted_capture (dev, img);
      break;

    case FPI_DEVICE_ACTION_IDENTIFY:
    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
    default:
      g_object_unref (img);
      break;
    }
}

/*
 * save_img: called once per completed post-init sequence with the raw 5356-byte
 * frame in transfer->buffer.  Implements the per-touch turn flow:
 *
 *  1. No finger / zero frame → close any open turn, clear waiting_for_lift if
 *     set, arm for next touch. A lift mid-turn (settle or evaluate window) thus
 *     immediately ends the turn and re-arms.
 *  2. Finger present, waiting_for_lift or not armed → ignore.
 *  3. Finger present, armed → open turn; skip frames until FINGER_SETTLE_MS.
 *  4. Past settle, still within TURN_TIMEOUT_MS → run quality gate once per frame.
 *     First frame that passes: enhance, submit to on_frame_accepted, done.
 *  5. TURN_TIMEOUT_MS elapsed without a valid frame → report absent, set
 *     waiting_for_lift so no new turn starts until the finger is actually lifted.
 */
static void
save_img (FpiUsbTransfer *transfer, FpDevice *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  FpiSsm *ssm = transfer->ssm;
  gboolean has_valid_data = valid_data (transfer);

  dump_frame_if_requested (self, transfer, count_nonzero_bytes (transfer));
  self->frame_reads_this_claim += 1;
  fp_dbg ("Frame reads in current claim: %u/%u",
          self->frame_reads_this_claim,
          EGIS0575_MAX_FRAMES_PER_CLAIM);

  if (self->stop)
    {
      fpi_ssm_jump_to_state (ssm, SM_DONE);
      return;
    }

  /* ---- Startup warmup: capture the idle baseline as the warm background. ----
   * Until a baseline exists, finger_detected compares against bg=0 and the hot
   * idle frame reads as a finger, deadlocking detection. Grab the first few
   * valid (non-zero) frames unconditionally; skip cold/zero frames (a zero
   * background fails to cancel the sensor's hot pixels). Don't run finger
   * detection until this completes. */
  if (self->background_warmup_remaining > 0)
    {
      if (has_valid_data)
        {
          /* Adopted-calibration recovery self-check: imaging after the
           * embedded-block upload must produce structured frames. A uniform
           * frame (e.g. the all-0xFF failure observed 2026-09-15) means the
           * adoption failed — discard it and fall back to patient re-reads
           * instead of hanging the action on a dead sensor pipeline. */
          if (self->cal_recovery_adopted)
            {
              if (frame_is_uniform (transfer))
                {
                  fp_warn ("Adopted calibration produced uniform frames; resuming calibration re-reads (%u/%d)",
                           self->cal_read_recoveries, EGIS0575_CAL_READ_MAX_RECOVERIES);
                  self->cal_recovery_adopted = FALSE;
                  g_clear_pointer (&self->calibration, g_free);
                  self->background_warmup_remaining = EGIS0575_BACKGROUND_WARMUP_FRAMES;
                  fpi_ssm_jump_to_state_delayed (ssm, SM_CAL_START,
                                                 EGIS0575_CAL_REREAD_DELAY_MS);
                  return;
                }
              fp_info ("Adopted calibration produces structured frames; recovery complete");
              self->cal_recovery_adopted = FALSE;
            }
          update_warm_background (self, transfer);
          self->background_warmup_remaining--;
          fp_dbg ("Background warmup: grabbed idle baseline (%u frame(s) left)",
                  self->background_warmup_remaining);
        }
      else
        {
          fp_dbg ("Background warmup: skipping cold/zero frame");
        }
      restart_for_next_poll (self, ssm, dev, "background warmup");
      return;
    }

  if (pgm_debug_enabled ())
    {
      gboolean present_now;

      pgm_debug_maybe_capture (self, transfer, has_valid_data);
      present_now = has_valid_data && finger_detected (self, transfer);
      if (!present_now && has_valid_data && frame_pixel_std (transfer) < EGIS0575_BG_UPDATE_MAX_STD)
        update_warm_background (self, transfer);
      report_finger_status (self, present_now, present_now ? "pgm debug present" : "pgm debug absent");
      restart_for_next_poll (self, ssm, dev, "pgm debug poll");
      return;
    }

  /* ---- No-finger path (zero frame or below detection threshold) ---- */
  if (!has_valid_data || !finger_detected (self, transfer))
    {
      if (has_valid_data && frame_pixel_std (transfer) < EGIS0575_BG_UPDATE_MAX_STD)
        update_warm_background (self, transfer);

      /* Lift during a verify turn ends collection: match what we have.
       * The action guard is load-bearing: residual probes from a cancelled
       * verify can still be present here, and finalize_verify on an enroll
       * or capture trips the current-action asserts without ever completing
       * that action (verify-complete hangs the enroll). */
      if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY &&
          self->verify_probes && self->verify_probes->len > 0)
        {
          fp_dbg ("Lift detected with %u verify probes; finalizing",
                  self->verify_probes->len);
          self->turn_open = FALSE;
          self->capture_armed = FALSE;
          self->waiting_for_lift = TRUE;
          report_finger_status (self, FALSE, "lift (verify finalize)");
          finalize_verify (self, ssm, dev);
          return;
        }

      /* Any lift clears the "wait for lift" block and arms a fresh turn. */
      if (self->waiting_for_lift)
        {
          self->waiting_for_lift = FALSE;
          self->waiting_for_lift_since = 0;
          fp_dbg ("Lift detected; re-arming for next touch");
        }

      /* Finger absent: dump any open turn so the next touch starts a fresh
       * settle + timeout window anchored on the new finger-present moment.
       * This covers a lift at any point during a turn (settle or evaluate
       * window), not just after a timeout. */
      self->turn_open = FALSE;
      self->capture_armed = TRUE;
      report_finger_status (self, FALSE, has_valid_data ? "below threshold" : "zero frame");
      if (has_valid_data && sensor_health_watchdog (self, transfer, ssm, dev))
        return;   /* watchdog redirected the SSM into recovery */
      /* Duty-cycled idle frame poll */
      fpi_ssm_jump_to_state_delayed (ssm, SM_INIT, idle_frame_delay_ms (self, dev));
      return;
    }

  /* ---- Finger present ---- */
  /* If they haven't lifted yet... */
  if (self->waiting_for_lift)
    {
      if (self->waiting_for_lift_since == 0)
        self->waiting_for_lift_since = g_get_monotonic_time ();

      /* Stuck-presence recovery: no real enrollment press keeps the finger on
       * the sensor for seconds after an accepted frame.  Both flat AGC drift
       * and structured drift (baseline corrupted by a faint earlier touch)
       * manifest as permanent finger presence — the timing signature is the
       * only reliable discriminator, so refresh the baseline unconditionally
       * after the timeout. */
      if (g_get_monotonic_time () - self->waiting_for_lift_since >
          EGIS0575_PHANTOM_LIFT_TIMEOUT_MS * 1000)
        {
          fp_warn ("Finger reported present for >%d ms after accept; refreshing background",
                   EGIS0575_PHANTOM_LIFT_TIMEOUT_MS);
          update_warm_background (self, transfer);
          self->waiting_for_lift = FALSE;
          self->waiting_for_lift_since = 0;
          self->turn_open = FALSE;
          self->capture_armed = TRUE;
          report_finger_status (self, FALSE, "stuck-presence recovery");
          restart_for_next_poll (self, ssm, dev, "stuck-presence recovery");
          return;
        }

      fp_dbg ("Finger still on sensor; waiting for lift before next turn");
      restart_for_next_poll (self, ssm, dev, "waiting for lift");
      return;
    }

  if (!self->turn_open)
    {
      self->finger_first_detected_time = g_get_monotonic_time ();
      self->turn_open = TRUE;
      report_finger_status (self, TRUE, "finger detected");
    }

  {
    gint64 elapsed = g_get_monotonic_time () - self->finger_first_detected_time;

    /* Turn timeout: fail, require lift before next attempt. A verify turn
     * with collected probes finalizes instead of discarding them. */
    if (elapsed > EGIS0575_TURN_TIMEOUT_MS * 1000)
      {
        /* Same action guard as the lift path: only a live VERIFY action
         * may consume collected probes; enroll/capture fall through to
         * the plain timeout handling below. */
        if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY &&
            self->verify_probes && self->verify_probes->len > 0)
          {
            fp_dbg ("Turn timed out with %u verify probes; finalizing",
                    self->verify_probes->len);
            self->turn_open = FALSE;
            self->capture_armed = FALSE;
            self->waiting_for_lift = TRUE;
            report_finger_status (self, FALSE, "turn timeout (verify finalize)");
            finalize_verify (self, ssm, dev);
            return;
          }

        fp_warn ("Turn timed out after %lld ms; waiting for lift before retry",
                 (long long) (elapsed / 1000));
        self->turn_open = FALSE;
        self->capture_armed = FALSE;
        self->waiting_for_lift = TRUE;
        report_finger_status (self, FALSE, "turn timeout");
        restart_for_next_poll (self, ssm, dev, "turn timeout");
        return;
      }

    /* Wait for a lift before arming for the FIRST capture */
    if (!self->capture_armed)
      {
        fp_dbg ("Capture not armed; ignoring finger frame");
        restart_for_next_poll (self, ssm, dev, "not armed");
        return;
      }

    /* Settle window: finger just landed, let the press stabilise. */
    if (elapsed < self->finger_settle_ms * 1000)
      {
        fp_dbg ("Finger settling (%lld ms / %u ms)", (long long) (elapsed / 1000), self->finger_settle_ms);
        restart_for_next_poll (self, ssm, dev, "finger settling");
        return;
      }

    /* Past settle, within turn window: evaluate quality. */
    {
      g_autoptr(FpImage) resized = NULL;
      guint grain_pct_x1000 = 0;
      guint ridge_pixels = 0;
      guint minutiae = 0;
      guint stretch_p5 = 0;
      guint stretch_p99 = 0;
      gboolean quality_ok;

      resized = create_processed_snapshot (self, transfer);

      /* stage2_snapshot_quality_ok normalises and applies stretch5 in-place. */
      quality_ok = stage2_snapshot_quality_ok (self, resized,
                                               &grain_pct_x1000,
                                               &ridge_pixels,
                                               &minutiae,
                                               &stretch_p5,
                                               &stretch_p99);

      fp_dbg ("Stage-2 at %lld ms: grain=%u.%03u%%/<%u.%03u%% ridge=%u/>%u minutiae=%u (%u..%u) => %s",
               (long long) (elapsed / 1000),
               grain_pct_x1000 / 1000, grain_pct_x1000 % 1000,
               EGIS0575_STAGE2_GRAIN_PCT_X1000 / 1000, EGIS0575_STAGE2_GRAIN_PCT_X1000 % 1000,
               ridge_pixels, EGIS0575_STAGE2_MIN_RIDGE_PIXELS,
               minutiae, EGIS0575_STAGE2_MIN_MINUTIAE, EGIS0575_STAGE2_MAX_MINUTIAE,
               quality_ok ? "accept" : "retry");

      maybe_write_live_frame_pgm (resized);

      if (quality_ok)
        {
          fp_dbg ("Frame accepted at %lld ms", (long long) (elapsed / 1000));
          /* resized is already normalised and stretch-enhanced by stage2. */

          if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY &&
              self->verify_probes)
            {
              /* Multi-frame verify: keep collecting qualifying frames within
               * the turn; finalize on target count, lift, or turn timeout.
               * turn_open/capture_armed stay set so the settle window is not
               * repeated per probe. */
              Egis0575MFeatureSet *fs = g_new (Egis0575MFeatureSet, 1);

              egis0575_m_extract (resized->data, resized->width, resized->height, fs);
              g_ptr_array_add (self->verify_probes, fs);
              dump_verify_image ("probe", self->verify_probes->len - 1, resized);
              fp_info ("Verify probe %u/%u captured (%d features)",
                       self->verify_probes->len, EGIS0575_VERIFY_PROBE_FRAMES, fs->n);

              /* Early exit: a probe that already clears the verdict needs no
               * further collection — keeps a successful press under ~2s. */
              if (self->verify_gallery &&
                  probe_matches_gallery (fs, self->verify_gallery,
                                         self->verify_gallery_n, NULL))
                {
                  fp_info ("Verify probe %u already matches; finalizing early",
                           self->verify_probes->len);
                  finalize_verify (self, ssm, dev);
                }
              else if (self->verify_probes->len >= EGIS0575_VERIFY_PROBE_FRAMES)
                finalize_verify (self, ssm, dev);
              else
                restart_for_next_poll (self, ssm, dev, "verify probe collection");
              return;
            }

          self->turn_open = FALSE;
          self->capture_armed = FALSE;
          on_frame_accepted (dev, g_steal_pointer (&resized));

          if (self->stop)
            fpi_ssm_jump_to_state (ssm, SM_DONE);
          else
            restart_capture_cycle (self, ssm, dev, "post-accept poll", EGIS0575_POST_CAPTURE_POLL_DELAY_MS);
          return;
        }
    }

    restart_for_next_poll (self, ssm, dev, "quality gate pending");
  }
}

/*
 * ==================== IO ====================
 */

/* All capture-loop transfers funnel through this wrapper so the driver knows
 * whether a transfer is still in flight. The cancel watchdog must not free
 * the SSM while one is: fpi_ssm_mark_failed() frees synchronously, and the
 * completed transfer's callback would then run on released memory.
 * (The real callback travels in gpointer user_data: function-pointer/object
 * pointer conversion is guaranteed by POSIX dlsym and by GCC/Clang, which
 * libfprint requires anyway.) */
static void
capture_transfer_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  FpiUsbTransferCallback real_cb = user_data;

  self->transfer_in_flight = FALSE;
  real_cb (transfer, dev, NULL, error);
}

/* Submit helper for every transfer of the capture loop. Routes the action
 * cancellable into the transfer so dev_cancel completes it promptly instead
 * of waiting out the full EGIS0575_TIMEOUT; the SM_DONE shutdown can outlive
 * the action, and fpi_device_get_cancellable() requires an ongoing one. */
static void
capture_transfer_submit (FpDeviceEgis0575       *self,
                         FpiUsbTransfer         *transfer,
                         FpDevice               *dev,
                         FpiUsbTransferCallback  callback)
{
  GCancellable *cancellable = NULL;

  if (fpi_device_get_current_action (dev) != FPI_DEVICE_ACTION_NONE)
    cancellable = fpi_device_get_cancellable (dev);

  self->transfer_in_flight = TRUE;
  fpi_usb_transfer_submit (transfer, EGIS0575_TIMEOUT, cancellable,
                           capture_transfer_cb, callback);
}

/* Timeout during a capture cycle: recycle the claim and restart, but only a
 * bounded number of times per action — a sensor that never produces a frame
 * is wedged and must fail cleanly (a completed frame resets the counter in
 * save_img). Returns TRUE if the SSM was failed. */
static gboolean
timeout_recover_or_fail (FpDeviceEgis0575 *self,
                         FpiSsm           *ssm,
                         FpDevice         *dev)
{
  self->timeout_recoveries++;
  if (self->timeout_recoveries > EGIS0575_TIMEOUT_RECOVERY_MAX)
    {
      fp_warn ("Sensor wedged mid-action (%u consecutive timeout recoveries); failing cleanly",
               self->timeout_recoveries);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "EH575 stopped responding mid-action; unplug and replug the sensor, then retry"));
      return TRUE;
    }

  fp_dbg ("Timeout at %s[%d], recycling claim and restarting capture (%u/%u)",
          packet_array_name (self->pkt_array),
          self->current_index,
          self->timeout_recoveries,
          EGIS0575_TIMEOUT_RECOVERY_MAX);
  restart_capture_cycle (self, ssm, dev, "timeout recovery", 0);
  return FALSE;
}

static void
resp_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  if (error)
    {
      if (!self->stop &&
          g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          timeout_recover_or_fail (self, transfer->ssm, dev);
          g_error_free (error);
          return;
        }

      fp_dbg ("Error occurred at index %d of %s array",
              self->current_index,
              packet_array_name (self->pkt_array));
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* Any successful transfer proves the transport is alive: reset the
   * consecutive-timeout recovery counter. */
  self->timeout_recoveries = 0;

  fp_dbg ("RX complete for %s[%d]: actual=%zd first-bytes=%02x %02x %02x %02x %02x %02x %02x",
          packet_array_name (self->pkt_array),
          self->current_index,
          transfer->actual_length,
          transfer->actual_length > 0 ? transfer->buffer[0] : 0,
          transfer->actual_length > 1 ? transfer->buffer[1] : 0,
          transfer->actual_length > 2 ? transfer->buffer[2] : 0,
          transfer->actual_length > 3 ? transfer->buffer[3] : 0,
          transfer->actual_length > 4 ? transfer->buffer[4] : 0,
          transfer->actual_length > 5 ? transfer->buffer[5] : 0,
          transfer->actual_length > 6 ? transfer->buffer[6] : 0);

  if (self->current_index == self->pkt_array_len - 1)
    {
      if (self->pkt_array[self->current_index].response_length == 5356)
        {
          /* Frame-capturing array complete (POST_CAL or POST_INIT): the last
           * packet's 5356-byte response is a raw frame. save_img evaluates it
           * and either accepts it or recycles the claim for another poll. */
          fp_dbg ("Completed %s sequence, passing frame to save_img",
                  packet_array_name (self->pkt_array));
          self->current_index = 0;
          save_img (transfer, dev);
          return;
        }
      else if (self->pkt_array == EGIS0575_PRE_INIT_PACKETS)
        {
          /* Pre-init complete — switch to post-init for the frame capture
           * (calibration-skip path only). */
          fp_dbg ("Completed pre-init sequence, switching to post-init");
          self->has_pre_init_run = TRUE;
          self->pkt_array = EGIS0575_POST_INIT_PACKETS;
          self->pkt_array_len = EGIS0575_POST_INIT_PACKETS_LENGTH;
          self->current_index = 0;
        }
    }
  else if (self->pkt_array == EGIS0575_POST_INIT_PACKETS &&
           self->current_index == 1 &&
           transfer->actual_length >= 7 &&
           transfer->buffer[4] == 0x01 &&
           transfer->buffer[5] == 0x01 &&
           transfer->buffer[6] == 0x01)
    {
      /*
       * EH575 semantics (Animeshz capture analysis): SIGE 01 01 01 at
       * POST_INIT[1] (the 60 01 fc poll) means "pre initialization required" —
       * the sensor lost its armed state.  Rerun the full pre-init sequence
       * instead of continuing the post-init array.
       */
      fp_dbg ("Pre-initialization required; switching back to pre-init");
      self->has_pre_init_run = FALSE;
      self->pkt_array = EGIS0575_PRE_INIT_PACKETS;
      self->pkt_array_len = EGIS0575_PRE_INIT_PACKETS_LENGTH;
      self->current_index = 0;
    }
  else
    {
      self->current_index += 1;
    }

  fpi_ssm_jump_to_state (transfer->ssm, SM_REQ);
}

static void
recv_resp (FpiSsm *ssm, FpDevice *dev, int response_length)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  fp_dbg ("Submitting bulk IN for %s[%d], expecting %d bytes",
          packet_array_name (self->pkt_array),
          self->current_index,
          response_length);

  fpi_usb_transfer_fill_bulk (transfer, EGIS0575_EPIN, response_length);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  capture_transfer_submit (self, transfer, dev, resp_cb);
}

static void
req_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  gboolean first_preinit = self->pkt_array == EGIS0575_PRE_INIT_PACKETS &&
                           self->current_index == 0 &&
                           !self->has_pre_init_run;

  if (error)
    {
      if (!self->stop &&
          g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          if (first_preinit)
            {
              if (self->startup_timeout_retries < EGIS0575_STARTUP_TIMEOUT_RECOVERY_MAX)
                {
                  self->startup_timeout_retries++;
                  fp_warn ("Timeout sending first pre-init packet; recycling claim and retrying startup (%u/%u)",
                           self->startup_timeout_retries,
                           EGIS0575_STARTUP_TIMEOUT_RECOVERY_MAX);
                  restart_capture_cycle (self,
                                         transfer->ssm,
                                         dev,
                                         "startup timeout recovery",
                                         EGIS0575_STARTUP_TIMEOUT_RECOVERY_DELAY_MS);
                  g_error_free (error);
                  return;
                }

              /* Claim-recycle retries exhausted; the device is transport-wedged.
               * Fail cleanly — the reliable recovery is unplug/replug. */
              fp_warn ("EH575 not responding to init after %u claim recycles; sensor is transport-wedged — unplug and replug it, then retry",
                       EGIS0575_STARTUP_TIMEOUT_RECOVERY_MAX);
              fpi_ssm_mark_failed (transfer->ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                             "EH575 did not respond to init (transport-wedged); unplug and replug the sensor, then retry"));
              g_error_free (error);
              return;
            }

          timeout_recover_or_fail (self, transfer->ssm, dev);
          g_error_free (error);
          return;
        }

      fp_dbg ("Error occurred sending packet at index %d of %s array",
              self->current_index,
              packet_array_name (self->pkt_array));
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* Any successful transfer proves the transport is alive. */
  self->timeout_recoveries = 0;

  if (first_preinit && self->startup_timeout_retries > 0)
    {
      fp_dbg ("Recovered startup after %u claim retr%s",
              self->startup_timeout_retries,
              self->startup_timeout_retries == 1 ? "y" : "ies");
      self->startup_timeout_retries = 0;
    }

  fpi_ssm_next_state (transfer->ssm);
}

static void
send_req (FpiSsm *ssm, FpDevice *dev, const Packet *pkt)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  fp_dbg ("Submitting bulk OUT for %s[%d/%d]: %02x %02x %02x len=%d expect=%d",
          packet_array_name (self->pkt_array),
          self->current_index,
          self->pkt_array_len,
          pkt->sequence[4],
          pkt->sequence[5],
          pkt->sequence[6],
          pkt->length,
          pkt->response_length);

  fpi_usb_transfer_fill_bulk_full (transfer, EGIS0575_EPOUT, pkt->sequence, pkt->length, NULL);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  capture_transfer_submit (self, transfer, dev, req_cb);
}

/*
 * ==================== Calibration helpers ====================
 */

static void
cal_packet_ssm_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  FpiUsbTransfer *transfer;
  const Packet *pkt;

  fpi_ssm_silence_debug (ssm);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case PACKET_SSM_REQ:
      pkt = &self->cal_pkt_array[self->cal_pkt_index];
      transfer = fpi_usb_transfer_new (dev);

      fpi_usb_transfer_fill_bulk_full (transfer, EGIS0575_EPOUT, pkt->sequence, pkt->length, NULL);

      transfer->ssm = ssm;
      transfer->short_is_error = TRUE;

      capture_transfer_submit (self, transfer, dev, fpi_ssm_usb_transfer_cb);
      break;

    case PACKET_SSM_RESP:
      transfer = fpi_usb_transfer_new (dev);

      fpi_usb_transfer_fill_bulk (transfer, EGIS0575_EPIN,
                                  self->cal_pkt_array[self->cal_pkt_index].response_length);

      transfer->ssm = ssm;
      transfer->short_is_error = TRUE;

      capture_transfer_submit (self, transfer, dev, fpi_ssm_usb_transfer_cb);
      self->cal_pkt_index++;
      break;

    case PACKET_SSM_LOOP:
      if (self->cal_pkt_index == self->cal_pkt_len)
        fpi_ssm_mark_completed (ssm);
      else
        fpi_ssm_jump_to_state (ssm, PACKET_SSM_REQ);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
start_cal_packet_array (FpDeviceEgis0575 *self,
                        FpiSsm           *ssm,
                        const Packet     *array,
                        int               len)
{
  self->cal_pkt_array = array;
  self->cal_pkt_len = len;
  self->cal_pkt_index = 0;

  fpi_ssm_start_subsm (ssm,
                       fpi_ssm_new_full (FP_DEVICE (self),
                                         cal_packet_ssm_run_state,
                                         PACKET_SSM_DONE,
                                         PACKET_SSM_DONE,
                                         "egis0575-cal-packets"));
}

/* Best-effort variant for the shutdown sequence: transfer errors are ignored
 * so a wedged sensor cannot fail an action that already completed. */
static void
shutdown_ignoring_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  if (error)
    g_error_free (error);

  fpi_ssm_next_state (transfer->ssm);
}

static void
shutdown_packet_ssm_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  FpiUsbTransfer *transfer;
  const Packet *pkt;

  fpi_ssm_silence_debug (ssm);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case PACKET_SSM_REQ:
      pkt = &self->cal_pkt_array[self->cal_pkt_index];
      transfer = fpi_usb_transfer_new (dev);
      fpi_usb_transfer_fill_bulk_full (transfer, EGIS0575_EPOUT, pkt->sequence, pkt->length, NULL);
      transfer->ssm = ssm;
      capture_transfer_submit (self, transfer, dev, shutdown_ignoring_cb);
      break;

    case PACKET_SSM_RESP:
      transfer = fpi_usb_transfer_new (dev);
      fpi_usb_transfer_fill_bulk (transfer, EGIS0575_EPIN,
                                  self->cal_pkt_array[self->cal_pkt_index].response_length);
      transfer->ssm = ssm;
      capture_transfer_submit (self, transfer, dev, shutdown_ignoring_cb);
      self->cal_pkt_index++;
      break;

    case PACKET_SSM_LOOP:
      if (self->cal_pkt_index == self->cal_pkt_len)
        fpi_ssm_mark_completed (ssm);
      else
        fpi_ssm_jump_to_state (ssm, PACKET_SSM_REQ);
      break;

    default:
      g_assert_not_reached ();
    }
}

/* Calibration-chain submit helpers. Known trade-off: unlike the capture
 * loop's req/resp callbacks (which route USB timeouts through
 * timeout_recover_or_fail), any error here — including a transient timeout —
 * fails the whole action. Accepted because the calibration chain runs once
 * at action start; the recovery is simply the user re-running the action. */
static void
cal_send (FpiSsm                *ssm,
          FpDevice              *dev,
          const unsigned char   *sequence,
          int                    length,
          FpiUsbTransferCallback callback)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk_full (transfer, EGIS0575_EPOUT, (unsigned char *) sequence, length, NULL);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  capture_transfer_submit (self, transfer, dev, callback);
}

static void
cal_read (FpiSsm                *ssm,
          FpDevice              *dev,
          int                    length,
          FpiUsbTransferCallback callback)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (transfer, EGIS0575_EPIN, length);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  capture_transfer_submit (self, transfer, dev, callback);
}

/* Status-poll responses: wait until resp[5] reaches the phase-specific value
 * (bounded by EGIS0575_CAL_POLL_MAX_ITERS so a wedged sensor fails cleanly). */
static void
cal_status_poll_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (self->cal_poll_iters++ > EGIS0575_CAL_POLL_MAX_ITERS)
    {
      /* Same bounded patience as broken calibration reads: post-session
       * states can stall the phase polls entirely (observed 2026-09-15/16
       * after rapid session clusters); re-running the chain after a pause
       * recovers. With a calibration block already in hand (adopted or
       * cached) SM_CAL_START takes the fast re-arm path. */
      if (self->cal_read_recoveries < EGIS0575_CAL_READ_MAX_RECOVERIES)
        {
          self->cal_read_recoveries++;
          self->cal_poll_iters = 0;
          fp_warn ("Calibration status poll did not converge; retrying chain (%u/%d)",
                   self->cal_read_recoveries, EGIS0575_CAL_READ_MAX_RECOVERIES);
          fpi_ssm_jump_to_state_delayed (transfer->ssm, SM_CAL_START,
                                         EGIS0575_CAL_REREAD_DELAY_MS);
          return;
        }
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "calibration status poll did not converge"));
      return;
    }

  switch (fpi_ssm_get_cur_state (transfer->ssm))
    {
    case SM_CAL_POLL_2_RESP:
      if (transfer->buffer[5] != 0x05)
        fpi_ssm_jump_to_state (transfer->ssm, SM_CAL_POLL_2_REQ);
      else
        fpi_ssm_next_state (transfer->ssm);
      break;

    case SM_CAL_POLL_4_RESP:
      if (transfer->buffer[5] != 0x00)
        fpi_ssm_jump_to_state (transfer->ssm, SM_CAL_POLL_4_REQ);
      else
        fpi_ssm_next_state (transfer->ssm);
      break;

    case SM_RESET_POLL_RESP:
      if (transfer->buffer[5] == 0x00)
        fpi_ssm_jump_to_state (transfer->ssm, SM_RESET_POLL_REQ);
      else
        fpi_ssm_next_state (transfer->ssm);
      break;

    default:
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "calibration poll callback in wrong state"));
      break;
    }
}

/*
 * ==================== SSM loopback ====================
 */

static void
ssm_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SM_CAL_START:
      if (self->cal_skip)
        {
          fp_dbg ("Calibration skipped (EGIS0575_SKIP_CALIBRATION=1)");
          /* Anchor the idle-reinit watchdog at open time: last_full_init_time
           * is otherwise only written by the full-calibration path
           * (SM_CAL_ACK), so in skip mode it stays 0 and the first valid
           * empty frame after >10 min of host uptime would fire one spurious
           * preventive re-init. */
          self->last_full_init_time = g_get_monotonic_time ();
          self->has_pre_init_run = FALSE;
          fpi_ssm_jump_to_state (ssm, SM_INIT);
        }
      else if (self->calibration)
        {
          /* Cal block already cached this session: re-arm the sensor by
           * re-uploading it (POST_RESET -> 73 14 ec -> write -> ack). */
          fp_dbg ("Calibration cached; re-arming sensor");
          fpi_ssm_jump_to_state (ssm, SM_POST_RESET);
        }
      else
        {
          fp_dbg ("Reading calibration block from sensor");
          self->cal_poll_iters = 0;
          fpi_ssm_next_state (ssm);
        }
      break;

    case SM_CAL_PHASE_1:
      start_cal_packet_array (self, ssm,
                              EGIS0575_CAL_PHASE_1_PACKETS,
                              EGIS0575_CAL_PHASE_1_PACKETS_LENGTH);
      break;

    case SM_CAL_POLL_2_REQ:
      cal_send (ssm, dev, EGIS0575_CAL_POLL_2.sequence, EGIS0575_CAL_POLL_2.length,
                fpi_ssm_usb_transfer_cb);
      break;

    case SM_CAL_POLL_2_RESP:
      cal_read (ssm, dev, 7, cal_status_poll_cb);
      break;

    case SM_CAL_PHASE_3:
      start_cal_packet_array (self, ssm,
                              EGIS0575_CAL_PHASE_3_PACKETS,
                              EGIS0575_CAL_PHASE_3_PACKETS_LENGTH);
      break;

    case SM_CAL_POLL_4_REQ:
      cal_send (ssm, dev, EGIS0575_CAL_POLL_4.sequence, EGIS0575_CAL_POLL_4.length,
                fpi_ssm_usb_transfer_cb);
      break;

    case SM_CAL_POLL_4_RESP:
      cal_read (ssm, dev, 7, cal_status_poll_cb);
      break;

    case SM_CAL_PHASE_5:
      start_cal_packet_array (self, ssm,
                              EGIS0575_CAL_PHASE_5_PACKETS,
                              EGIS0575_CAL_PHASE_5_PACKETS_LENGTH);
      break;

    case SM_CAL_READ_REQ:
      /* Adopted-calibration recovery: the register phases have run and the
       * embedded block is already in place, so skip the sensor read (it would
       * return the invalidated all-zero block and overwrite the good copy)
       * and continue with the 97 reset + upload half of the chain. */
      if (self->cal_recovery_adopted)
        {
          fpi_ssm_jump_to_state (ssm, SM_PRE_RESET);
          break;
        }
      cal_send (ssm, dev, (const unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x72, 0x14, 0xec}, 7,
                fpi_ssm_usb_transfer_cb);
      break;

    case SM_CAL_READ_RESP:
      {
        FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

        self->calibration = g_malloc0 (EGIS0575_IMGSIZE);
        fpi_usb_transfer_fill_bulk_full (transfer, EGIS0575_EPIN, self->calibration,
                                         EGIS0575_IMGSIZE, NULL);

        transfer->ssm = ssm;
        transfer->short_is_error = TRUE;

        capture_transfer_submit (self, transfer, dev, fpi_ssm_usb_transfer_cb);
      }
      break;

    case SM_CAL_CHECK:
      {
        /* A broken read leaves a long run of identical trailing bytes (topni1
         * measured 0x3f); discard the block and fail the action cleanly. */
        unsigned char last_byte = self->calibration[EGIS0575_IMGSIZE - 1];
        gboolean broken = TRUE;

        for (int i = EGIS0575_IMGSIZE - 2; i > EGIS0575_IMGSIZE - EGIS0575_CAL_BROKEN_TAIL_RUN; i--)
          {
            if (self->calibration[i] != last_byte)
              {
                broken = FALSE;
                break;
              }
            }

        /* Head/tail fingerprint of every read: a stale idle frame misread as
         * the calibration block (FIFO residue) has a frame-like head, a
         * firmware-side invalid block does not — the bytes tell them apart. */
        fp_dbg ("Calibration block: head %02x %02x %02x %02x %02x %02x, tail %02x %02x %02x %02x%s",
                self->calibration[0], self->calibration[1], self->calibration[2],
                self->calibration[3], self->calibration[4], self->calibration[5],
                self->calibration[EGIS0575_IMGSIZE - 4], self->calibration[EGIS0575_IMGSIZE - 3],
                self->calibration[EGIS0575_IMGSIZE - 2], last_byte,
                broken ? " (broken)" : "");

        if (broken)
          {
            fp_warn ("Calibration block broken (tail run of 0x%02x; head %02x %02x %02x %02x %02x %02x)",
                     last_byte,
                     self->calibration[0], self->calibration[1], self->calibration[2],
                     self->calibration[3], self->calibration[4], self->calibration[5]);
            g_clear_pointer (&self->calibration, g_free);

            self->cal_read_recoveries++;

            /* Attempt 1: an immediate re-read absorbs transient corruption. */
            if (self->cal_read_recoveries == 1)
              {
                fp_warn ("Recovering: immediate re-read (1/%d)",
                         EGIS0575_CAL_READ_MAX_RECOVERIES);
                fpi_ssm_jump_to_state_delayed (ssm, SM_CAL_START,
                                               EGIS0575_STARTUP_TIMEOUT_RECOVERY_DELAY_MS);
                return;
              }

            /* Attempt 2: adopt the verify template's embedded calibration
             * block (v2 prints) — the host-side cache Windows keeps, carried
             * through fprintd's print storage. Runs the full open chain with
             * the sensor-side read replaced by the embedded copy (the
             * register phases and the 97 reset are required: the bare
             * claim-recycle re-arm path uploads onto a healthy sensor, which
             * this one is not — observed 2026-09-15: POST_RESET+upload alone
             * produced all-0xFF frames). The first captured frame is checked
             * for structure (save_img) so a failed adoption falls back to
             * the patient re-reads below instead of hanging the action. */
            if (self->cal_read_recoveries == 2 && self->embedded_cal)
              {
                fp_warn ("Recovering: uploading the template-embedded calibration block");
                self->calibration = g_memdup2 (self->embedded_cal, EGIS0575_IMGSIZE);
                self->cal_recovery_adopted = TRUE;
                self->cal_poll_iters = 0;
                fpi_ssm_jump_to_state (ssm, SM_CAL_PHASE_1);
                return;
              }

            /* Further attempts: the firmware rebuild takes seconds; keep
             * re-reading at 1 Hz until the block is back (bounded). */
            if (self->cal_read_recoveries <= EGIS0575_CAL_READ_MAX_RECOVERIES)
              {
                fp_warn ("Recovering: waiting for firmware self-calibration, re-read (%u/%d)",
                         self->cal_read_recoveries, EGIS0575_CAL_READ_MAX_RECOVERIES);
                fpi_ssm_jump_to_state_delayed (ssm, SM_CAL_START,
                                               EGIS0575_CAL_REREAD_DELAY_MS);
                return;
              }

            fp_warn ("Calibration read still broken after %d recoveries; retry later",
                     EGIS0575_CAL_READ_MAX_RECOVERIES);
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "broken calibration block, retry later"));
            return;
          }

        fp_dbg ("Calibration block read OK");
        fpi_ssm_next_state (ssm);
      }
      break;

    case SM_PRE_RESET:
      start_cal_packet_array (self, ssm,
                              EGIS0575_PRE_RESET_PACKETS,
                              EGIS0575_PRE_RESET_PACKETS_LENGTH);
      break;

    case SM_RESET_POLL_REQ:
      cal_send (ssm, dev, (const unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00}, 6,
                fpi_ssm_usb_transfer_cb);
      break;

    case SM_RESET_POLL_RESP:
      cal_read (ssm, dev, 7, cal_status_poll_cb);
      break;

    case SM_POST_RESET:
      start_cal_packet_array (self, ssm,
                              EGIS0575_POST_RESET_PACKETS,
                              EGIS0575_POST_RESET_PACKETS_LENGTH);
      break;

    case SM_CAL_ENTER_REQ:
      cal_send (ssm, dev, EGIS0575_CAL_ENTER_PACKET.sequence, EGIS0575_CAL_ENTER_PACKET.length,
                fpi_ssm_usb_transfer_cb);
      break;

    case SM_CAL_WRITE:
      {
        FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

        fpi_usb_transfer_fill_bulk_full (transfer, EGIS0575_EPOUT, self->calibration,
                                         EGIS0575_IMGSIZE, NULL);

        transfer->ssm = ssm;
        transfer->short_is_error = TRUE;

        capture_transfer_submit (self, transfer, dev, fpi_ssm_usb_transfer_cb);
      }
      break;

    case SM_CAL_ACK:
      cal_read (ssm, dev, 7, fpi_ssm_usb_transfer_cb);
      self->last_full_init_time = g_get_monotonic_time ();
      break;

    case SM_INIT:
      fp_dbg ("Starting capture");
      if (!self->cal_skip)
        {
          /* Calibration path: poll with the post-calibration table directly.
           * Its trailing 64 14 ec frame read feeds save_img (first pass is a
           * warm-up frame for the background baseline). */
          self->pkt_array = EGIS0575_POST_CALIBRATION_PACKETS;
          self->pkt_array_len = EGIS0575_POST_CALIBRATION_PACKETS_LENGTH;
          self->has_pre_init_run = TRUE;
        }
      else if (self->has_pre_init_run)
        {
          self->pkt_array = EGIS0575_POST_INIT_PACKETS;
          self->pkt_array_len = EGIS0575_POST_INIT_PACKETS_LENGTH;
        }
      else
        {
          self->pkt_array = EGIS0575_PRE_INIT_PACKETS;
          self->pkt_array_len = EGIS0575_PRE_INIT_PACKETS_LENGTH;
        }
      self->current_index = 0;

      fp_dbg ("Initial packet array: %s (claim_frames=%u/%u, active_width=%u, stage2: grain<%u.%03u%% minutiae=%u..%u ridge>%u)",
              packet_array_name (self->pkt_array),
              self->frame_reads_this_claim,
              EGIS0575_MAX_FRAMES_PER_CLAIM,
              self->active_width,
              EGIS0575_STAGE2_GRAIN_PCT_X1000 / 1000,
              EGIS0575_STAGE2_GRAIN_PCT_X1000 % 1000,
              EGIS0575_STAGE2_MIN_MINUTIAE,
              EGIS0575_STAGE2_MAX_MINUTIAE,
              EGIS0575_STAGE2_MIN_RIDGE_PIXELS);
      if (!self->has_pre_init_run && self->pkt_array == EGIS0575_PRE_INIT_PACKETS && self->current_index == 0)
        {
          fp_dbg ("Applying %u ms startup settle delay before first pre-init packet",
                  EGIS0575_STARTUP_SETTLE_DELAY_MS);
          fpi_ssm_jump_to_state_delayed (ssm, SM_START, EGIS0575_STARTUP_SETTLE_DELAY_MS);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      break;

    case SM_START:
      if (self->stop)
        {
          fp_dbg ("Stopping, completed capture");
          fpi_ssm_mark_completed (ssm);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      break;

    case SM_REQ:
      fp_dbg ("SSM_REQ using %s[%d/%d]",
              packet_array_name (self->pkt_array),
              self->current_index,
              self->pkt_array_len);

      send_req (ssm, dev, &self->pkt_array[self->current_index]);
      break;

    case SM_RESP:
      fp_dbg ("SSM_RESP waiting on %s[%d/%d]",
              packet_array_name (self->pkt_array),
              self->current_index,
              self->pkt_array_len);
      recv_resp (ssm, dev, self->pkt_array[self->current_index].response_length);
      break;

    case SM_DONE:
      fp_dbg ("Capture loop stopping; shutting sensor down");
      self->cal_pkt_array = EGIS0575_SHUTDOWN_PACKETS;
      self->cal_pkt_len = EGIS0575_SHUTDOWN_PACKETS_LENGTH;
      self->cal_pkt_index = 0;
      fpi_ssm_start_subsm (ssm,
                           fpi_ssm_new_full (FP_DEVICE (dev),
                                             shutdown_packet_ssm_run_state,
                                             PACKET_SSM_DONE,
                                             PACKET_SSM_DONE,
                                             "egis0575-shutdown"));
      break;

    case SM_SHUTDOWN:
      /* shutdown subsm completed (best-effort); finish the loop */
      fpi_ssm_jump_to_state (ssm, SM_FINISH);
      break;

    case SM_FINISH:
      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
loop_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  GCancellable *cancellable = NULL;
  g_autoptr(GError) cancelled = NULL;

  self->running = FALSE;
  self->capture_ssm = NULL;
  self->cancel_watchdog_armed = FALSE;
  /* A watchdog source left pending (re-armed just before wind-down) must not
   * fire into a later action. */
  g_clear_pointer (&self->cancel_watchdog_source, g_source_destroy);

  if (error)
    {
      /* A cancel that raced an in-flight USB error must still surface as a
       * cancellation, not as a device failure. */
      if (fpi_device_get_current_action (dev) != FPI_DEVICE_ACTION_NONE)
        cancellable = fpi_device_get_cancellable (dev);

      if (cancellable && g_cancellable_set_error_if_cancelled (cancellable, &cancelled))
        {
          fp_dbg ("Capture loop completed after cancellation (superseded error: %s)",
                  error->message);
          g_error_free (error);
          fpi_device_action_error (dev, g_steal_pointer (&cancelled));
          return;
        }

      fp_dbg ("Capture loop completed with error: %s", error->message);
      fpi_device_action_error (dev, error);
      return;
    }

  if (fpi_device_get_current_action (dev) != FPI_DEVICE_ACTION_NONE)
    cancellable = fpi_device_get_cancellable (dev);

  if (cancellable && g_cancellable_set_error_if_cancelled (cancellable, &cancelled))
    {
      fp_dbg ("Capture loop completed after cancellation");
      fpi_device_action_error (dev, g_steal_pointer (&cancelled));
      return;
    }

  fp_dbg ("Capture loop completed cleanly");
}

/*
 * ==================== Top-level command callback & meta-data ====================
 */

static void
reset_action_state (FpDeviceEgis0575 *self)
{
  self->stop = FALSE;
  self->finger_reported = FALSE;
  self->capture_armed = FALSE;
  self->waiting_for_lift = FALSE;
  self->waiting_for_lift_since = 0;
  self->frame_counter = 0;
  self->pgm_debug_counter = 0;
  self->pgm_debug_last_capture_time = 0;
  self->frame_reads_this_claim = 0;
  self->turn_open = FALSE;
  self->has_pre_init_run = FALSE;
  self->enroll_sim_rejects = 0;
  self->startup_timeout_retries = 0;
  self->timeout_recoveries = 0;
  self->transfer_in_flight = FALSE;
  self->cal_poll_iters = 0;
  self->cal_read_recoveries = 0;
  self->cal_recovery_adopted = FALSE;
  self->weak_press_events = 0;
  self->weak_press_window_start = 0;
  self->background_warmup_remaining = EGIS0575_BACKGROUND_WARMUP_FRAMES;
  clear_background (self);

  /* Cancel does not touch verify_probes (only dev_verify rebuilds the array
   * and finalize_verify drains it), so a cancelled verify leaves collected
   * probes behind; the next action in the same claim session must not see
   * them or its no-finger/turn-timeout paths would finalize a VERIFY on an
   * enroll/capture action. */
  if (self->verify_probes)
    g_ptr_array_set_size (self->verify_probes, 0);
}

static void start_capture_action_cb (FpDevice *dev, gpointer user_data);

static void
start_capture_action (FpDevice *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  FpiSsm *ssm;

  if (self->running)
    {
      /* The previous action's capture loop is still running its SM_DONE
       * shutdown chain (actions complete while it drains). Two live loops
       * would share cal_pkt_array/transfer_in_flight/capture_ssm and
       * corrupt each other, so wait for the old loop to wind down — but
       * only up to EGIS0575_ACTION_START_WAIT_MAX_MS; a loop that never
       * drains must surface as an error, not an unbounded wait. */
      if (self->action_start_wait_since == 0)
        self->action_start_wait_since = g_get_monotonic_time ();

      if (g_get_monotonic_time () - self->action_start_wait_since >
          (gint64) EGIS0575_ACTION_START_WAIT_MAX_MS * 1000)
        {
          fp_warn ("Previous capture loop still draining after %d ms; failing action",
                   EGIS0575_ACTION_START_WAIT_MAX_MS);
          self->action_start_wait_since = 0;
          fpi_device_action_error (dev,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_BUSY,
                                                             "previous capture loop did not wind down"));
          return;
        }

      fpi_device_add_timeout (dev, 50, start_capture_action_cb, NULL, NULL);
      return;
    }

  self->action_start_wait_since = 0;

  ssm = fpi_ssm_new_full (dev, ssm_run_state, SM_STATES_NUM, SM_STATES_NUM,
                          "egis0575-capture");
  reset_action_state (self);
  self->last_finger_activity_ms = g_get_monotonic_time () / 1000;
  self->capture_ssm = ssm;
  fpi_ssm_start (ssm, loop_complete);
  self->running = TRUE;
}

static void
start_capture_action_cb (FpDevice *dev, gpointer user_data)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  FpiDeviceAction action = fpi_device_get_current_action (dev);

  /* The action that asked for this start may have completed (cancelled)
   * while the old loop was still draining; never start a capture loop
   * without a live action to drive it. Drop the wait anchor too, so the
   * next action's bounded wait starts from its own request time. */
  if (action != FPI_DEVICE_ACTION_ENROLL &&
      action != FPI_DEVICE_ACTION_VERIFY &&
      action != FPI_DEVICE_ACTION_CAPTURE)
    {
      self->action_start_wait_since = 0;
      return;
    }

  start_capture_action (dev);
}

static void
dev_cancel (FpDevice *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  fp_dbg ("Cancel requested (running=%d stop=%d)", self->running, self->stop);
  self->stop = TRUE;
  if (self->running && !self->cancel_watchdog_armed)
    {
      self->cancel_watchdog_armed = TRUE;
      self->cancel_watchdog_source =
        fpi_device_add_timeout (dev, EGIS0575_CANCEL_WATCHDOG_MS, cancel_watchdog_cb, NULL, NULL);
    }
}

/* fprintd 1.94.5 can abandon a cancelled action without ever sending close
 * (killed client mid-verify leaves the claim stuck). Force the capture
 * loop to a terminal state so the action completes and fprintd unblocks.
 *
 * Never force-fail while a transfer is in flight: fpi_ssm_mark_failed()
 * frees the SSM synchronously and the transfer's callback would then touch
 * released memory. Transfers are submitted with the action cancellable and
 * carry a hard EGIS0575_TIMEOUT, so the in-flight window is short; re-arm
 * and wait it out instead. */
static void
cancel_watchdog_cb (FpDevice *dev, gpointer user_data)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  /* The firing timeout is one-shot and gets destroyed on return; drop our
   * reference so loop_complete never destroys a stale source. */
  self->cancel_watchdog_source = NULL;

  if (!self->cancel_watchdog_armed)
    return;

  if (self->running && self->capture_ssm)
    {
      if (self->transfer_in_flight)
        {
          fp_dbg ("Cancel watchdog: transfer still in flight; re-arming");
          self->cancel_watchdog_source =
            fpi_device_add_timeout (dev, EGIS0575_CANCEL_WATCHDOG_MS, cancel_watchdog_cb, NULL, NULL);
          return;
        }
      fp_warn ("Cancel watchdog: capture loop still running 1s after cancel; forcing failure");
      fpi_ssm_mark_failed (self->capture_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "cancel watchdog: loop did not wind down"));
    }
  self->cancel_watchdog_armed = FALSE;
}

static void
open_settle_cb (FpDevice *dev, gpointer user_data)
{
  fpi_device_open_complete (dev, NULL);
}

static void
dev_open (FpDevice *dev)
{
  GError *error = NULL;
  GUsbDevice *usb_dev = fpi_device_get_usb_device (dev);

  fp_dbg ("Opening EH575 device (active_width=%u)", FPI_DEVICE_EGIS0575 (dev)->active_width);
  fp_dbg ("Claiming interface %d", EGIS0575_INTERFACE);
  if (!g_usb_device_claim_interface (usb_dev,
                                     EGIS0575_INTERFACE,
                                     0,
                                     &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  /* Keep open lightweight; startup hardening happens via a short settle delay
   * before the first pre-init packet and claim-recycle on timeout. The settle
   * is asynchronous so the device thread's main context stays responsive. */
  fp_dbg ("Settling %u ms after claim before first action", EGIS0575_STARTUP_SETTLE_DELAY_MS);
  fpi_device_add_timeout (dev, EGIS0575_STARTUP_SETTLE_DELAY_MS, open_settle_cb, NULL, NULL);
}

static void
dev_close (FpDevice *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  if (self->running)
    {
      /* Releasing the interface with in-flight URBs can wedge the sensor
       * firmware at the USB level (observed as device-not-accepting-address
       * after close-mid-transfer). Set the stop flag and complete the close
       * once the capture loop has wound down. */
      fp_dbg ("Close while capture loop running; deferring interface release");
      self->stop = TRUE;
      self->close_poll_retries = 0;
      fpi_device_add_timeout (dev, EGIS0575_CLOSE_POLL_INTERVAL_MS, close_poll_cb, NULL, NULL);
      return;
    }

  egis0575_finish_close (dev);
}

/* Re-checks the capture loop while a deferred close waits for it. The loop
 * is bounded by the cancel watchdog, so this normally ends within a poll or
 * two; the retry cap is the hard backstop that keeps a wedged sensor from
 * making the device impossible to close and reopen. */
static void
close_poll_cb (FpDevice *dev, gpointer user_data)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  self->close_poll_retries++;

  if (self->running &&
      self->close_poll_retries < EGIS0575_CLOSE_POLL_MAX_RETRIES)
    {
      fp_dbg ("Close: capture loop still running, waiting");
      fpi_device_add_timeout (dev, EGIS0575_CLOSE_POLL_INTERVAL_MS, close_poll_cb, NULL, NULL);
      return;
    }

  if (self->running)
    fp_warn ("Close: capture loop still running after %u polls (%u ms); releasing interface anyway",
             self->close_poll_retries,
             self->close_poll_retries * EGIS0575_CLOSE_POLL_INTERVAL_MS);

  egis0575_finish_close (dev);
}

/* All heap buffers the driver keeps across an action. Used by the close
 * path and by dispose, so a device dropped without a clean close (client
 * killed, test harness) cannot leak the ~110 KB enroll gallery et al.
 * The calibration block is deliberately NOT dropped here: it survives close
 * as a host-side cache (Windows behaves the same way, protocol.md §4C) —
 * SM_CAL_START re-uploads it instead of re-running the read-out chain.
 * It is dropped by the sensor-health watchdog, the broken-block check,
 * and dispose. The verify-feedback cache likewise survives close (it only
 * pays off across actions within the fprintd process) and is dropped by
 * dispose. */
static void
egis0575_release_buffers (FpDeviceEgis0575 *self)
{
  clear_background (self);
  g_clear_pointer (&self->verify_probes, g_ptr_array_unref);
  g_clear_pointer (&self->verify_gallery, g_free);
  self->verify_gallery_n = 0;
  g_clear_pointer (&self->embedded_cal, g_free);
  g_clear_pointer (&self->enroll_feats, g_free);
}

static void
egis0575_finish_close (FpDevice *dev)
{
  GError *error = NULL;
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  egis0575_release_buffers (self);
  fp_dbg ("Releasing interface %d", EGIS0575_INTERFACE);
  g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                  EGIS0575_INTERFACE,
                                  0,
                                  &error);

  fpi_device_close_complete (dev, error);
}

static void
dev_enroll (FpDevice *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  fp_dbg ("Enroll requested");
  g_clear_pointer (&self->enroll_feats, g_free);
  self->enroll_feats = g_new0 (Egis0575MFeatureSet, EGIS0575_ENROLL_FRAMES);
  self->enroll_stage = 0;

  start_capture_action (dev);
}

/* Dump verify-time probes and the stored gallery to PGM files for offline
 * matcher analysis (EGIS0575_VERIFY_DUMP_DIR). */
static void
dump_verify_image (const gchar *kind, guint index, FpImage *img)
{
  const gchar *dir = g_getenv ("EGIS0575_VERIFY_DUMP_DIR");
  g_autofree gchar *path = NULL;
  g_autoptr(GError) error = NULL;

  if (!dir || !dir[0])
    return;

  g_mkdir_with_parents (dir, 0700);

  path = g_strdup_printf ("%s/%s-%03u.pgm", dir, kind, index);
  if (!write_image_pgm (img, path, &error))
    fp_warn ("verify dump: failed to write %s: %s", path, error->message);
}

static void
dump_verify_gallery (FpDevice *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  if (!g_getenv ("EGIS0575_VERIFY_DUMP_DIR"))
    return;

  for (guint i = 0; i < self->verify_gallery_n; i++)
    fp_dbg ("gallery frame %u: %d features", i, self->verify_gallery[i].n);
}

static void
dev_verify (FpDevice *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  FpPrint *verify_print = NULL;
  g_autoptr(GVariant) stored = NULL;

  fp_dbg ("Verify requested (multi-frame probes)");
  g_clear_pointer (&self->verify_probes, g_ptr_array_unref);
  self->verify_probes = g_ptr_array_new_with_free_func (g_free);

  g_clear_pointer (&self->verify_gallery, g_free);
  self->verify_gallery_n = 0;

  fpi_device_get_verify_data (dev, &verify_print);
  g_object_get (verify_print, "fpi-data", &stored, NULL);
  g_clear_pointer (&self->embedded_cal, g_free);
  if (!stored || !unpack_feature_frames (stored, &self->verify_gallery,
                                         &self->verify_gallery_n,
                                         &self->embedded_cal))
    {
      /* Fail fast: running a whole capture turn only to report a bad
       * template afterwards wastes seconds and confuses the user. */
      fp_warn ("Verify print has no valid feature gallery; re-enroll");
      g_clear_pointer (&self->verify_probes, g_ptr_array_unref);
      fpi_device_verify_complete (dev,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                            "enrolled print has no usable EH575 template (outdated or invalid format); delete it and re-enroll"));
      return;
    }

  /* The match verdict needs at least EGIS0575_M_AGREE_FRAMES gallery frames
   * scoring above AGREE_SCORE; fewer can never match (enroll always pools
   * EGIS0575_ENROLL_FRAMES frames, so this only catches hand-crafted
   * fpi-data blobs). Fail fast instead of failing every verify silently. */
  if (self->verify_gallery_n < EGIS0575_M_AGREE_FRAMES)
    {
      fp_warn ("Verify print has only %u gallery frame(s); minimum is %d; re-enroll",
               self->verify_gallery_n, EGIS0575_M_AGREE_FRAMES);
      g_clear_pointer (&self->verify_probes, g_ptr_array_unref);
      fpi_device_verify_complete (dev,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                            "enrolled print has too few EH575 frames to ever match; delete it and re-enroll"));
      return;
    }

  /* Template feedback: swap in the adapted gallery if this print has one
   * cached, otherwise seed the cache from the enrolled template so later
   * verifies pick up taught frames. */
  if (self->verify_feedback)
    {
      g_autoptr(GBytes) enrolled_id = g_variant_get_data_as_bytes (stored);
      Egis0575FeedbackEntry *entry = feedback_cache_lookup (self, enrolled_id);

      if (entry)
        {
          g_clear_pointer (&self->verify_gallery, g_free);
          self->verify_gallery = g_malloc (entry->n_frames * sizeof (Egis0575MFeatureSet));
          memcpy (self->verify_gallery, entry->frames,
                  entry->n_frames * sizeof (Egis0575MFeatureSet));
          self->verify_gallery_n = entry->n_frames;
          if (entry->n_frames > entry->n_enrolled)
            fp_info ("Verify using feedback-adapted gallery (%u enrolled + %u taught frames)",
                     entry->n_enrolled, entry->n_frames - entry->n_enrolled);
        }
      else
        {
          feedback_cache_store (self, enrolled_id,
                                self->verify_gallery, self->verify_gallery_n);
        }
    }

  dump_verify_gallery (dev);

  start_capture_action (dev);
}

static void
dev_capture (FpDevice *dev)
{
  fp_dbg ("Capture requested");
  start_capture_action (dev);
}

static const FpIdEntry id_table[] = {
  { .vid = 0x1c7a, .pid = 0x0575, },
  { .vid = 0,      .pid = 0, },
};

static void
fpi_device_egis0575_init (FpDeviceEgis0575 *self)
{
  const gchar *env = g_getenv ("EGIS0575_ACTIVE_WIDTH");

  self->cal_skip = (g_strcmp0 (g_getenv ("EGIS0575_SKIP_CALIBRATION"), "1") == 0);
  self->active_width = EGIS0575_SENSOR_ACTIVE_WIDTH_DEFAULT;
  self->enroll_sim_threshold = EGIS0575_ENROLL_SIM_THRESHOLD_DEFAULT;

  if (env && env[0])
    {
      gint64 parsed = g_ascii_strtoll (env, NULL, 10);

      if (parsed >= 1 && parsed <= EGIS0575_SENSOR_STRIDE_X)
        self->active_width = (guint) parsed;
      else
        fp_warn ("Ignoring EGIS0575_ACTIVE_WIDTH=%s (must be 1..%d)",
                 env, EGIS0575_SENSOR_STRIDE_X);
    }

  env = g_getenv ("EGIS0575_ENROLL_SIM_THRESHOLD");
  if (env && env[0])
    {
      gint64 parsed = g_ascii_strtoll (env, NULL, 10);

      if (parsed >= 0 && parsed <= G_MAXINT)
        self->enroll_sim_threshold = (gint) parsed;
      else
        fp_warn ("Ignoring EGIS0575_ENROLL_SIM_THRESHOLD=%s (must be 0..%d)",
                 env, G_MAXINT);
    }

  self->finger_settle_ms = EGIS0575_FINGER_SETTLE_MS_DEFAULT;
  env = g_getenv ("EGIS0575_FINGER_SETTLE_MS");
  if (env && env[0])
    {
      gint64 parsed = g_ascii_strtoll (env, NULL, 10);

      if (parsed >= 0 && parsed <= EGIS0575_TURN_TIMEOUT_MS)
        self->finger_settle_ms = (guint) parsed;
      else
        fp_warn ("Ignoring EGIS0575_FINGER_SETTLE_MS=%s (must be 0..%d)",
                 env, EGIS0575_TURN_TIMEOUT_MS);
    }

  self->verify_feedback = (g_strcmp0 (g_getenv ("EGIS0575_VERIFY_FEEDBACK"), "0") != 0);

  self->padded_img_width = ((self->active_width + 3) / 4) * 4;
}

static void
fpi_device_egis0575_dispose (GObject *object)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (object);

  g_clear_pointer (&self->calibration, g_free);
  g_clear_pointer (&self->feedback_cache, g_ptr_array_unref);
  egis0575_release_buffers (self);

  G_OBJECT_CLASS (fpi_device_egis0575_parent_class)->dispose (object);
}

static void
fpi_device_egis0575_class_init (FpDeviceEgis0575Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = fpi_device_egis0575_dispose;

  dev_class->id = "egis0575";
  dev_class->full_name = "LighTuning Technology Inc. EgisTec EH575";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = EGIS0575_ENROLL_FRAMES;
  dev_class->temp_hot_seconds = -1;

  dev_class->open = dev_open;
  dev_class->close = dev_close;
  dev_class->cancel = dev_cancel;
  dev_class->enroll = dev_enroll;
  dev_class->verify = dev_verify;
  dev_class->capture = dev_capture;

  fpi_device_class_auto_initialize_features (dev_class);
  dev_class->features &= ~FP_DEVICE_FEATURE_IDENTIFY;
}
