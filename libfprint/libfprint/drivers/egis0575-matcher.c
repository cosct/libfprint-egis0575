/*
 * EgisTec EH575 matcher: reverse-engineered from EgisTouchFPEngine0575.dll
 * (0x180009560..0x180015800).
 *
 * True Windows DLL pipeline:
 *  1. 15x15 flat-field local mean subtraction + std normalization (* 64.0).
 *  2. Separable 2D Gaussian blur (0x180013d60) using fir_taps[config[0]-10]
 *     = fir_taps[6] ({361, 1415, 3757, 6748, 8205, 6749, 3757, 1415, 361}).
 *  3. Span-2 (x+-2, y+-2) [3, 10, 3]/64 Scharr derivatives (0x18000f6b0,
 *     0x180014530) for gx, gy, Ixx, Ixy, Iyy.
 *  4. Hessian determinant keypoint detector (0x180014660..0x180014c80):
 *     det(H) = (Ixx * Iyy - Ixy^2) / 16.0, polarity class type = (Ixx < 0),
 *     3x3 local maxima with border margin 10 and NMS radius 6.
 *  5. 11x11 Gaussian-weighted (w_row * w_col at 0x1800431d0) 360-degree
 *     sliding-window gradient orientation estimator (0x180014ec0).
 *  6. Multi-scale (2x2, 3x3, 4x4 rotated grids, R=9) 3-channel (I, gx_rot,
 *     gy_rot) all-pairs ordinal rank descriptor (0x180015200, 0x180015600):
 *     (C(4,2) + C(9,2) + C(16,2)) * 3 = 486 bits stored in desc[0..60],
 *     with Hessian polarity type (0 or 1) stored in desc[63].
 *  7. MatchMinutia (0x18000a610): same-polarity Lowe-ratio + strict-budget
 *     1-NN candidates -> top-3 360-degree orientation mode peaks -> rotate
 *     gallery coordinates by -mode_ang around (cx, cy) -> translation
 *     clustering -> second-pass spatial neighbor rescue & contradiction
 *     rejection (0x18000a150, 0x18000aeb0).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "egis0575-matcher.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 1D Gaussian smoothing kernel fir_taps[6] (sigma=1.6, config[0]=16) at
 * 0x180043070 in EgisTouchFPEngine0575.dll; sum = 32768 (Q15). */
static const int fir6[9] = {
  361, 1415, 3757, 6748, 8205, 6749, 3757, 1415, 361
};

/* Separable 11x11 Gaussian window weights for 360-deg orientation histogram
 * (0x1800431d0 in EgisTouchFPEngine0575.dll). */
static const double w_row[11] = {
  57, 118, 207, 308, 392, 425, 392, 308, 207, 118, 57
};
static const double w_col[11] = {
  53, 109, 191, 285, 362, 392, 362, 285, 191, 109, 53
};

static inline int
clampi (int v, int lo, int hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

/* Span-2 [3, 10, 3]/64 Scharr derivatives (DLL 0x18000f6b0). */
static void
scharr_span2 (const double *src, int w, int h, double *dx_out, double *dy_out)
{
  for (int y = 0; y < h; y++)
    {
      int ym2 = y >= 2 ? y - 2 : 1;
      int yp2 = y < h - 2 ? y + 2 : (y == h - 2 ? y + 1 : y);

      for (int x = 0; x < w; x++)
        {
          int xm2 = x >= 2 ? x - 2 : 1;
          int xp2 = x < w - 2 ? x + 2 : (x == w - 2 ? x + 1 : x);

          if (dx_out)
            {
              double c1 = (src[ym2 * w + xp2] - src[ym2 * w + xm2]) +
                          (src[yp2 * w + xp2] - src[yp2 * w + xm2]);
              double c2 = src[y * w + xp2] - src[y * w + xm2];
              dx_out[y * w + x] = (3.0 * c1 + 10.0 * c2) / 64.0;
            }
          if (dy_out)
            {
              double c1 = (src[yp2 * w + xm2] - src[ym2 * w + xm2]) +
                          (src[yp2 * w + xp2] - src[ym2 * w + xp2]);
              double c2 = src[yp2 * w + x] - src[ym2 * w + x];
              dy_out[y * w + x] = (3.0 * c1 + 10.0 * c2) / 64.0;
            }
        }
    }
}

/* Multi-scale (N=2,3,4) rotated grid sampler + all-pairs ordinal rank
 * packing (DLL 0x180015200 + 0x180015600). Produces 486 bits in desc[0..60]
 * and stores polarity type in desc[63]. */
static int
sample_grid_and_pack (const double *blur,
                      const double *gx,
                      const double *gy,
                      int           w,
                      int           h,
                      double        fx,
                      double        fy,
                      int           angle_deg,
                      uint8_t       type_bit,
                      uint8_t       desc[EGIS0575_M_DESC_BYTES])
{
  memset (desc, 0, EGIS0575_M_DESC_BYTES);
  double rad = angle_deg * M_PI / 180.0;
  double ca = cos (rad), sa = sin (rad);
  const int R = 9; /* DLL config[0x10] = 9 */
  int bit_idx = 0;

  for (int N = 2; N <= 4; N++)
    {
      double cell_step = (2.0 * R) / (double) N;
      double ch_vals[3][16];

      for (int r = 0; r < N; r++)
        {
          double u0 = -R + r * cell_step;
          double u1 = u0 + cell_step;

          for (int c = 0; c < N; c++)
            {
              double v0 = -R + c * cell_step;
              double v1 = v0 + cell_step;
              double sum_I = 0.0, sum_dx = 0.0, sum_dy = 0.0;
              int cnt = 0;

              for (double u = u0 + 0.25; u < u1; u += 0.5)
                {
                  for (double v = v0 + 0.25; v < v1; v += 0.5)
                    {
                      double px = fx + u * ca - v * sa;
                      double py = fy + u * sa + v * ca;
                      int ix = (int) round (px);
                      int iy = (int) round (py);

                      if (ix < 0 || ix >= w || iy < 0 || iy >= h)
                        continue;
                      sum_I += blur[iy * w + ix];
                      sum_dx += gx[iy * w + ix];
                      sum_dy += gy[iy * w + ix];
                      cnt++;
                    }
                }
              if (cnt < 12)
                return 0;

              int cell_id = r * N + c;
              double mx = sum_dx / cnt;
              double my = sum_dy / cnt;
              ch_vals[0][cell_id] = sum_I / cnt;
              ch_vals[1][cell_id] = mx * ca + my * sa;
              ch_vals[2][cell_id] = mx * sa - my * ca;
            }
        }

      int n_cells = N * N;
      for (int ch = 0; ch < 3; ch++)
        {
          for (int i = 0; i < n_cells; i++)
            {
              for (int j = i + 1; j < n_cells; j++)
                {
                  if (ch_vals[ch][i] > ch_vals[ch][j])
                    desc[bit_idx >> 3] |= (uint8_t) (1u << (bit_idx & 7));
                  bit_idx++;
                }
            }
        }
    }

  desc[63] = type_bit;
  return 1;
}

typedef struct
{
  int     x, y;
  double  v;
  uint8_t t;
} CandPt;

void
egis0575_m_extract (const uint8_t       *img,
                    int                  w,
                    int                  h,
                    Egis0575MFeatureSet *out)
{
  out->n = 0;
  if (!img || w < 32 || h < 32)
    return;

  size_t npix = (size_t) w * (size_t) h;
  double *flat = calloc (npix, sizeof (double));
  double *tmp = calloc (npix, sizeof (double));
  double *blur = calloc (npix, sizeof (double));
  double *gx = calloc (npix, sizeof (double));
  double *gy = calloc (npix, sizeof (double));
  double *ixx = calloc (npix, sizeof (double));
  double *ixy = calloc (npix, sizeof (double));
  double *iyy = calloc (npix, sizeof (double));
  double *det = calloc (npix, sizeof (double));
  uint8_t *type = calloc (npix, 1);
  CandPt *cands = malloc (sizeof (CandPt) * 2048);

  if (!flat || !tmp || !blur || !gx || !gy || !ixx || !ixy || !iyy || !det || !type || !cands)
    {
      free (flat); free (tmp); free (blur); free (gx); free (gy);
      free (ixx); free (ixy); free (iyy); free (det); free (type); free (cands);
      return;
    }

  /* 1. 15x15 local mean subtraction + global std normalization */
  double mean_all = 0.0, sq_all = 0.0;
  for (int y = 0; y < h; y++)
    {
      int ya = clampi (y - 7, 0, h - 1);
      int yb = clampi (y + 7, 0, h - 1);
      for (int x = 0; x < w; x++)
        {
          int xa = clampi (x - 7, 0, w - 1);
          int xb = clampi (x + 7, 0, w - 1);
          double s = 0.0;
          int c = 0;
          for (int yy = ya; yy <= yb; yy++)
            for (int xx = xa; xx <= xb; xx++)
              {
                s += img[yy * w + xx];
                c++;
              }
          double v = (double) img[y * w + x] - s / c;
          flat[y * w + x] = v;
          mean_all += v;
          sq_all += v * v;
        }
    }

  double n_d = (double) npix;
  double var = sq_all / n_d - (mean_all / n_d) * (mean_all / n_d);
  double std = sqrt (var > 0.0 ? var : 0.0);
  for (size_t i = 0; i < npix; i++)
    flat[i] = (flat[i] / (std + 1e-9)) * 64.0;

  /* 2. Separable 2D Gaussian blur with fir6 (DLL 0x180013d60) */
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        double acc = 0.0;
        for (int k = -4; k <= 4; k++)
          acc += flat[y * w + clampi (x + k, 0, w - 1)] * fir6[k + 4];
        tmp[y * w + x] = acc / 32768.0;
      }

  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        double acc = 0.0;
        for (int k = -4; k <= 4; k++)
          acc += tmp[clampi (y + k, 0, h - 1) * w + x] * fir6[k + 4];
        blur[y * w + x] = acc / 32768.0;
      }

  /* 3. Span-2 Scharr first and second derivatives (DLL 0x180014530) */
  scharr_span2 (blur, w, h, gx, gy);
  scharr_span2 (gx, w, h, ixx, ixy);
  scharr_span2 (gy, w, h, NULL, iyy);

  /* 4. Hessian determinant and ridge/valley polarity (DLL 0x180014660) */
  for (size_t i = 0; i < npix; i++)
    {
      det[i] = (ixx[i] * iyy[i] - ixy[i] * ixy[i]) / 16.0;
      type[i] = (ixx[i] < 0.0) ? 1 : 0;
    }

  /* 5. 3x3 local maxima of det(H) with border margin 10 */
  int nc = 0;
  for (int y = 10; y < h - 10; y++)
    {
      for (int x = 10; x < w - 10; x++)
        {
          double v = det[y * w + x];
          if (v <= 0.1)
            continue;
          int ok = 1;
          for (int dy = -1; dy <= 1 && ok; dy++)
            for (int dx = -1; dx <= 1; dx++)
              if ((dx || dy) && det[(y + dy) * w + x + dx] > v)
                {
                  ok = 0;
                  break;
                }
          if (ok && nc < 2048)
            {
              cands[nc].x = x;
              cands[nc].y = y;
              cands[nc].v = v;
              cands[nc].t = type[y * w + x];
              nc++;
            }
        }
    }

  /* Sort candidates descending by Hessian response */
  for (int i = 0; i < nc; i++)
    for (int j = i + 1; j < nc; j++)
      if (cands[j].v > cands[i].v)
        {
          CandPt t = cands[i];
          cands[i] = cands[j];
          cands[j] = t;
        }

  /* 6. NMS + 360-deg orientation + 486-bit multi-scale ordinal descriptor */
  for (int i = 0; i < nc && out->n < EGIS0575_M_MAX_FEATURES; i++)
    {
      int x = cands[i].x;
      int y = cands[i].y;
      int near = 0;

      for (int k = 0; k < out->n; k++)
        {
          int dx = x - (int) out->f[k].x;
          int dy = y - (int) out->f[k].y;
          if (dx * dx + dy * dy < 36)
            {
              near = 1;
              break;
            }
        }
      if (near)
        continue;

      double hist_gx[360] = { 0 };
      double hist_gy[360] = { 0 };
      for (int yy = 0; yy < 11; yy++)
        {
          for (int xx = 0; xx < 11; xx++)
            {
              int py = y - 5 + yy;
              int px = x - 5 + xx;
              double wgt = w_row[yy] * w_col[xx];
              double vx = gx[py * w + px] * wgt;
              double vy = gy[py * w + px] * wgt;
              if (vx == 0.0 && vy == 0.0)
                continue;
              int ang = (int) round (atan2 (vy, vx) * 180.0 / M_PI);
              if (ang < 0)
                ang += 360;
              if (ang >= 360)
                ang -= 360;
              hist_gx[ang] += vx;
              hist_gy[ang] += vy;
            }
        }

      double best_mag = -1.0, best_wx = 0.0, best_wy = 0.0;
      for (int a = 0; a < 360; a++)
        {
          double wx = 0.0, wy = 0.0;
          for (int d = -18; d < 18; d++)
            {
              int idx = (a + d + 360) % 360;
              wx += hist_gx[idx];
              wy += hist_gy[idx];
            }
          double mag = wx * wx + wy * wy;
          if (mag > best_mag)
            {
              best_mag = mag;
              best_wx = wx;
              best_wy = wy;
            }
        }
      if (best_mag <= 0.0)
        continue;

      int dom_ang = (int) round (atan2 (best_wy, best_wx) * 180.0 / M_PI);
      if (dom_ang < 0)
        dom_ang += 360;
      if (dom_ang >= 360)
        dom_ang -= 360;

      uint8_t desc[EGIS0575_M_DESC_BYTES];
      if (!sample_grid_and_pack (blur, gx, gy, w, h, x, y, dom_ang, cands[i].t, desc))
        continue;

      Egis0575MFeature *f = &out->f[out->n++];
      f->x = (uint16_t) x;
      f->y = (uint16_t) y;
      f->orient = (uint8_t) ((dom_ang / 2) % EGIS0575_M_N_ORIENT);
      memcpy (f->desc, desc, EGIS0575_M_DESC_BYTES);
    }

  free (flat); free (tmp); free (blur); free (gx); free (gy);
  free (ixx); free (ixy); free (iyy); free (det); free (type); free (cands);
}

/* Popcount over the 486 descriptor bits (bytes 0..60). */
static int
hamming486 (const uint8_t a[EGIS0575_M_DESC_BYTES],
            const uint8_t b[EGIS0575_M_DESC_BYTES])
{
  int s = 0;
  for (int i = 0; i < 61; i++)
    s += __builtin_popcount ((unsigned int) (a[i] ^ b[i]));
  return s;
}

static inline int
adiff360 (int a, int b)
{
  int d = abs (a - b) % 360;
  return d > 180 ? 360 - d : d;
}

static int
eval_pose (const Egis0575MFeatureSet *a,
           const Egis0575MFeatureSet *b,
           int                        mode_ang,
           const int                 *p_i,
           const int                 *p_j,
           const int                 *p_h,
           int                        np,
           int                        pos_tol_px,
           int                        ang_tol_deg,
           int                       *out_votes)
{
  double rad = -mode_ang * M_PI / 180.0;
  double ca = cos (rad), sa = sin (rad);
  double cx0 = 104.0, cy0 = 52.0;
  double bx_rot[EGIS0575_M_MAX_FEATURES];
  double by_rot[EGIS0575_M_MAX_FEATURES];
  int bang_rot[EGIS0575_M_MAX_FEATURES];

  for (int j = 0; j < b->n; j++)
    {
      double dx = (double) b->f[j].x - cx0;
      double dy = (double) b->f[j].y - cy0;
      bx_rot[j] = ca * dx - sa * dy + cx0;
      by_rot[j] = sa * dx + ca * dy + cy0;
      bang_rot[j] = ((int) b->f[j].orient * 2 - mode_ang + 360) % 360;
    }

  int best_votes = 0, best_dx = 0, best_dy = 0, best_raw = 0;
  for (int k = 0; k < np; k++)
    {
      int da = ((int) b->f[p_j[k]].orient * 2 - (int) a->f[p_i[k]].orient * 2 + 360) % 360;
      if (adiff360 (da, mode_ang) > ang_tol_deg)
        continue;
      int dx0 = (int) round (bx_rot[p_j[k]] - (double) a->f[p_i[k]].x);
      int dy0 = (int) round (by_rot[p_j[k]] - (double) a->f[p_i[k]].y);
      int v = 0, sc = 0;
      uint8_t u_j[EGIS0575_M_MAX_FEATURES] = { 0 };

      for (int m = 0; m < np; m++)
        {
          int dm = ((int) b->f[p_j[m]].orient * 2 - (int) a->f[p_i[m]].orient * 2 + 360) % 360;
          if (adiff360 (dm, mode_ang) > ang_tol_deg)
            continue;
          int dx = (int) round (bx_rot[p_j[m]] - (double) a->f[p_i[m]].x);
          int dy = (int) round (by_rot[p_j[m]] - (double) a->f[p_i[m]].y);
          if (abs (dx - dx0) <= pos_tol_px &&
              abs (dy - dy0) <= pos_tol_px &&
              !u_j[p_j[m]])
            {
              u_j[p_j[m]] = 1;
              v++;
              sc += (140 - p_h[m]);
            }
        }
      if (v > best_votes || (v == best_votes && sc > best_raw))
        {
          best_votes = v;
          best_raw = sc;
          best_dx = dx0;
          best_dy = dy0;
        }
    }

  if (best_votes < 3)
    {
      if (out_votes)
        *out_votes = best_votes;
      return 0;
    }

  uint8_t inlier_i[EGIS0575_M_MAX_FEATURES] = { 0 };
  uint8_t used_j[EGIS0575_M_MAX_FEATURES] = { 0 };
  int total_votes = 0, total_score = 0;

  for (int m = 0; m < np; m++)
    {
      int dm = ((int) b->f[p_j[m]].orient * 2 - (int) a->f[p_i[m]].orient * 2 + 360) % 360;
      if (adiff360 (dm, mode_ang) > ang_tol_deg)
        continue;
      int dx = (int) round (bx_rot[p_j[m]] - (double) a->f[p_i[m]].x);
      int dy = (int) round (by_rot[p_j[m]] - (double) a->f[p_i[m]].y);
      if (abs (dx - best_dx) <= pos_tol_px &&
          abs (dy - best_dy) <= pos_tol_px &&
          !used_j[p_j[m]])
        {
          inlier_i[p_i[m]] = 1;
          used_j[p_j[m]] = 1;
          total_votes++;
          total_score += (140 - p_h[m]);
        }
    }

  /* Second-pass same-polarity spatial neighbor rescue & contradiction check
   * (DLL 0x18000a150 + 0x18000aeb0). */
  int sp_contra = 0;
  for (int i = 0; i < a->n; i++)
    {
      if (inlier_i[i])
        continue;
      double tx = (double) a->f[i].x + best_dx;
      double ty = (double) a->f[i].y + best_dy;
      int near_j = -1;
      double near_d2 = 36.0; /* radius 6px (0x24 in DLL 0x18000a31e) */

      for (int j = 0; j < b->n; j++)
        {
          if (a->f[i].desc[63] != b->f[j].desc[63])
            continue;
          double d2 = (bx_rot[j] - tx) * (bx_rot[j] - tx) +
                      (by_rot[j] - ty) * (by_rot[j] - ty);
          if (d2 < near_d2)
            {
              near_d2 = d2;
              near_j = j;
            }
        }
      if (near_j >= 0)
        {
          int h = hamming486 (a->f[i].desc, b->f[near_j].desc);
          int ang_err = adiff360 ((int) a->f[i].orient * 2, bang_rot[near_j]);
          if (!used_j[near_j] && ang_err <= 18 && h <= 115)
            {
              used_j[near_j] = 1;
              total_votes++;
              total_score += (140 - h);
            }
          else if (ang_err >= 45 || h >= 190)
            {
              sp_contra++;
            }
        }
    }

  if (total_votes < EGIS0575_M_MIN_MATCHED || sp_contra >= total_votes)
    {
      if (out_votes)
        *out_votes = 0;
      return 0;
    }

  if (out_votes)
    *out_votes = total_votes;
  int final_score = total_score - sp_contra * 25;
  return final_score > 0 ? final_score : 0;
}

static int
score_one_dir (const Egis0575MFeatureSet *a,
               const Egis0575MFeatureSet *b,
               int                        pos_tol_px,
               double                     angle_tol_deg,
               int                       *out_votes)
{
  if (!a || !b || a->n < EGIS0575_M_MIN_FEATURES || b->n < EGIS0575_M_MIN_FEATURES)
    {
      if (out_votes)
        *out_votes = 0;
      return 0;
    }

  int p_i[EGIS0575_M_MAX_FEATURES];
  int p_j[EGIS0575_M_MAX_FEATURES];
  int p_h[EGIS0575_M_MAX_FEATURES];
  int np = 0;
  int ang_tol = (int) round (angle_tol_deg);

  for (int i = 0; i < a->n && i < EGIS0575_M_MAX_FEATURES; i++)
    {
      int bj = -1, bh = 9999, sh = 9999;
      for (int j = 0; j < b->n && j < EGIS0575_M_MAX_FEATURES; j++)
        {
          if (a->f[i].desc[63] != b->f[j].desc[63])
            continue;
          int h = hamming486 (a->f[i].desc, b->f[j].desc);
          if (h < bh)
            {
              sh = bh;
              bh = h;
              bj = j;
            }
          else if (h < sh)
            {
              sh = h;
            }
        }
      /* DLL 0x18000a8bb: Lowe ratio OR strict Hamming budget */
      if (bj >= 0 && bh <= EGIS0575_M_HAMMING_BUDGET &&
          (bh * 1024 < sh * 880 || bh <= 90))
        {
          p_i[np] = i;
          p_j[np] = bj;
          p_h[np] = bh;
          np++;
        }
    }

  int ang_w[180] = { 0 };
  for (int idx = 0; idx < 180; idx++)
    {
      int c = idx * 2;
      int w = 0;
      for (int k = 0; k < np; k++)
        {
          int da = ((int) b->f[p_j[k]].orient * 2 - (int) a->f[p_i[k]].orient * 2 + 360) % 360;
          if (adiff360 (da, c) <= ang_tol)
            w += (140 - p_h[k]);
        }
      ang_w[idx] = w;
    }

  int best_score = 0, best_votes = 0;
  uint8_t tried[180] = { 0 };
  for (int peak = 0; peak < 3; peak++)
    {
      int b_idx = -1, b_w = 0;
      for (int idx = 0; idx < 180; idx++)
        {
          if (!tried[idx] && ang_w[idx] > b_w)
            {
              b_w = ang_w[idx];
              b_idx = idx;
            }
        }
      if (b_idx < 0)
        break;
      for (int d = -7; d <= 7; d++)
        tried[(b_idx + d + 180) % 180] = 1;

      int v = 0;
      int s = eval_pose (a, b, b_idx * 2, p_i, p_j, p_h, np,
                         pos_tol_px, ang_tol, &v);
      if (s > best_score || (s == best_score && v > best_votes))
        {
          best_score = s;
          best_votes = v;
        }
    }

  if (out_votes)
    *out_votes = best_votes;
  return best_score;
}

int
egis0575_m_score_ex (const Egis0575MFeatureSet *probe,
                     const Egis0575MFeatureSet *gallery,
                     int                       *votes_out,
                     int                        pos_tol_px,
                     double                     angle_tol_deg)
{
  int v1 = 0, v2 = 0;
  int s1 = score_one_dir (probe, gallery, pos_tol_px, angle_tol_deg, &v1);
  int s2 = score_one_dir (gallery, probe, pos_tol_px, angle_tol_deg, &v2);

  if (s1 >= s2)
    {
      if (votes_out)
        *votes_out = v1;
      return s1;
    }
  else
    {
      if (votes_out)
        *votes_out = v2;
      return s2;
    }
}

int
egis0575_m_score (const Egis0575MFeatureSet *probe,
                  const Egis0575MFeatureSet *gallery,
                  int                       *votes_out)
{
  return egis0575_m_score_ex (probe, gallery, votes_out,
                              EGIS0575_M_POS_TOL_PX,
                              EGIS0575_M_ANGLE_TOL_DEG);
}
