/*
 * Offline verify-feedback evaluation: replay the driver's verify-time
 * template feedback loop over the collected verify-run datasets and report
 * its FAR/FRR impact.
 *
 * Method (mirrors dev_verify/finalize_verify/feedback_teach in
 * libfprint/libfprint/drivers/egis0575.c):
 *   - runs are grouped into sessions by identical enrolled gallery
 *     (byte hash of the gallery-*.pgm dumps);
 *   - within a session, runs are replayed chronologically (dir mtime);
 *     per run, probes are scored in capture order with the driver's
 *     verdict (best >= MATCH_THRESHOLD && agree >= AGREE_FRAMES frames
 *     >= AGREE_SCORE), early exit on the first matching probe;
 *   - a run whose best score clears MATCH_THRESHOLD + MARGIN teaches its
 *     best probe into the session gallery (dedup gate = enroll-sim
 *     threshold, cap MAX_FRAMES, FIFO among taught frames only);
 *   - baseline (12 enrolled frames only) and feedback-adapted scores are
 *     reported side by side, plus a cross-session matrix against the
 *     final adapted galleries (conservative FAR proxy: the same finger
 *     may be re-enrolled across sessions, inflating cross scores).
 *
 * Runs without gallery dumps can still be scored when an enrolled
 * template is supplied via --storage (examples/ test-storage.variant):
 * runs newer than the storage file's mtime are assigned to it.
 *
 * Usage:
 *   egis0575-feedback-eval [datasets-dir] [--storage test-storage.variant]
 *
 * Copyright (C) 2026 cosct <cosct@outlook.com>
 * Copyright (C) 2026 fprintdriver research contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <glib.h>

#include "../libfprint/libfprint/drivers/egis0575-matcher.h"

/* Driver constants (egis0575.c) replicated for the simulation. */
#define TEACH_MARGIN       30   /* EGIS0575_VERIFY_FEEDBACK_MIN_MARGIN */
#define GALLERY_MAX_FRAMES 16   /* EGIS0575_VERIFY_FEEDBACK_MAX_FRAMES */
#define DEDUP_GATE         650  /* EGIS0575_ENROLL_SIM_THRESHOLD_DEFAULT */

#define TEACH_SCORE (EGIS0575_M_MATCH_THRESHOLD + TEACH_MARGIN)

#define MAX_GALLERY 64   /* unpack_feature_frames tolerates up to 64 */
#define MAX_PROBES   32

typedef struct
{
  char               name[128];
  long long          mtime;
  int                n_probes;
  Egis0575MFeatureSet probes[MAX_PROBES];
  int                session;     /* index into sessions[], -1 = unscorable */
  /* results */
  int                base_best;
  int                base_agree_at_best;
  int                fb_best;
  int                fb_match;    /* verdict under feedback simulation */
  int                taught_this_run;
} Run;

typedef struct
{
  uint64_t           hash;
  char               label[128];
  int                n_enrolled;
  Egis0575MFeatureSet enrolled[MAX_GALLERY];
  /* simulation state */
  int                n_frames;
  Egis0575MFeatureSet frames[MAX_GALLERY];
  int                n_runs;
  int                taught_total;
} Session;

static Run      runs[256];
static int      n_runs;
static Session  sessions[16];
static int      n_sessions;

/* ---------------------------------------------------------------- PGM IO */

static int
pgm_next_token (FILE *f, char *buf, size_t len)
{
  int c;

  for (;;)
    {
      c = fgetc (f);
      if (c == EOF)
        return 0;
      if (c == '#')
        {
          while (c != EOF && c != '\n')
            c = fgetc (f);
          continue;
        }
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
        break;
    }

  size_t n = 0;
  while (c != EOF && c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '#')
    {
      if (n + 1 < len)
        buf[n++] = (char) c;
      c = fgetc (f);
    }
  if (c != EOF)
    ungetc (c, f);
  buf[n] = '\0';
  return n > 0;
}

static unsigned char *
load_pgm (const char *path, int *w, int *h)
{
  FILE *f = fopen (path, "rb");
  char tok[32];
  int maxv;

  if (!f)
    return NULL;
  if (!pgm_next_token (f, tok, sizeof (tok)) || strcmp (tok, "P5") != 0 ||
      !pgm_next_token (f, tok, sizeof (tok)) || (*w = atoi (tok)) <= 0 ||
      !pgm_next_token (f, tok, sizeof (tok)) || (*h = atoi (tok)) <= 0 ||
      !pgm_next_token (f, tok, sizeof (tok)) || (maxv = atoi (tok)) != 255)
    {
      fclose (f);
      return NULL;
    }
  {
    int t = fgetc (f);
    if (t == '\r')
      {
        int t2 = fgetc (f);
        if (t2 != '\n')
          ungetc (t2, f);
      }
  }
  size_t npix = (size_t) *w * *h;
  unsigned char *data = malloc (npix);
  if (!data || fread (data, 1, npix, f) != npix)
    {
      free (data);
      fclose (f);
      return NULL;
    }
  fclose (f);
  return data;
}

/* ------------------------------------------------------------ simulations */

static int
score_vs (const Egis0575MFeatureSet *probe, const Egis0575MFeatureSet *frame)
{
  return egis0575_m_score (probe, frame, NULL);
}

/* Driver verdict for one probe: best + agree over the current gallery. */
static int
probe_match (const Egis0575MFeatureSet *probe,
             const Egis0575MFeatureSet *gallery, int n_gallery,
             int *best_out, int *agree_out)
{
  int best = 0, agree = 0;

  for (int g = 0; g < n_gallery; g++)
    {
      int s = score_vs (probe, &gallery[g]);
      if (s > best)
        best = s;
      if (s >= EGIS0575_M_AGREE_SCORE)
        agree++;
    }
  *best_out = best;
  *agree_out = agree;
  return best >= EGIS0575_M_MATCH_THRESHOLD && agree >= EGIS0575_M_AGREE_FRAMES;
}

/* feedback_teach(): append probe to the session gallery under the driver's
 * rails (dedup gate, taught-only FIFO at the frame cap). */
static int
teach (Session *s, const Egis0575MFeatureSet *probe)
{
  if (s->n_enrolled >= GALLERY_MAX_FRAMES)
    return 0;

  for (int i = 0; i < s->n_frames; i++)
    if (score_vs (probe, &s->frames[i]) >= DEDUP_GATE)
      return 0;

  if (s->n_frames >= GALLERY_MAX_FRAMES)
    {
      int n_taught = s->n_frames - s->n_enrolled;
      if (n_taught > 1)
        memmove (&s->frames[s->n_enrolled], &s->frames[s->n_enrolled + 1],
                 (n_taught - 1) * sizeof (Egis0575MFeatureSet));
      s->n_frames--;
    }

  s->frames[s->n_frames++] = *probe;
  return 1;
}

static uint64_t
fnv1a_file (const char *path, uint64_t h)
{
  FILE *f = fopen (path, "rb");
  unsigned char buf[4096];
  size_t n;

  if (!f)
    return h;
  while ((n = fread (buf, 1, sizeof (buf), f)) > 0)
    for (size_t i = 0; i < n; i++)
      {
        h ^= buf[i];
        h *= 1099511628211ULL;
      }
  fclose (f);
  return h;
}

static int
cmp_runs_by_mtime (const void *a, const void *b)
{
  const Run *ra = a, *rb = b;
  return (ra->mtime > rb->mtime) - (ra->mtime < rb->mtime);
}

/* ------------------------------------------------------- storage template */

/* Parse one enrolled print out of the examples' test-storage.variant:
 * vardict{key -> ay("FP3" + store((issbymsmsia{sv}v)))}, where the trailing
 * variant wraps the driver's fpi-data "(yaa(qqyay)ay)" (v2) or
 * "(yaa(qqyay))" (v1). Mirrors fp_print_deserialize. Returns the number of
 * frames parsed, or <= 0. */
static int
load_storage_gallery (const char *path, Session *s)
{
  g_autoptr(GError) error = NULL;
  gchar *contents = NULL;
  gsize length = 0;
  GVariant *dict, *val;
  int n = 0;

  if (!g_file_get_contents (path, &contents, &length, &error))
    {
      fprintf (stderr, "storage: %s\n", error->message);
      return -1;
    }
  dict = g_variant_new_from_bytes (G_VARIANT_TYPE_VARDICT,
                                   g_bytes_new_take (contents, length), FALSE);

  GVariantIter iter;
  gchar *key;

  g_variant_iter_init (&iter, dict);
  while (g_variant_iter_loop (&iter, "{sv}", &key, &val))
    {
      gsize plen = 0;
      const guchar *pdata = g_variant_get_fixed_array (val, &plen, 1);
      g_autoptr(GBytes) pbytes = NULL;
      g_autoptr(GVariant) raw = NULL, value = NULL, print_data = NULL, fpi = NULL, frames = NULL;
      guint8 version;
      guint nf;

      fprintf (stderr, "storage key '%s': plen=%zu\n", key, plen);

      if (!pdata || plen < 4 || memcmp (pdata, "FP3", 3) != 0)
        continue;

      pbytes = g_bytes_new (pdata + 3, plen - 3);
      raw = g_variant_new_from_bytes (G_VARIANT_TYPE ("(issbymsmsia{sv}v)"),
                                      pbytes, FALSE);
      if (!raw)
        continue;
      value = g_variant_get_normal_form (raw);

      {
        const gchar *driver, *dev_id, *user, *desc;
        gboolean stored;
        guint8 finger;
        gint type, julian;

        g_variant_get (value, "(i&s&sbymsmsi@a{sv}@v)",
                       &type, &driver, &dev_id, &stored, &finger,
                       &user, &desc, &julian, NULL, &print_data);
      }
      fpi = g_variant_get_variant (print_data);

      /* fp_print_serialize boxes print->data twice ("v" slot + explicit
       * g_variant_new_variant); fp_print_deserialize peels one layer via
       * get_child_value. Peel any remaining variant wrappers here. */
      while (g_variant_is_of_type (fpi, G_VARIANT_TYPE ("v")))
        {
          GVariant *inner = g_variant_get_variant (fpi);
          g_variant_unref (fpi);
          fpi = inner;
        }

      fprintf (stderr, "  fpi-data type: %s\n", g_variant_get_type_string (fpi));

      if (g_variant_is_of_type (fpi, G_VARIANT_TYPE ("(yaa(qqyay)ay)")))
        {
          GVariant *cal;

          g_variant_get (fpi, "(y@aa(qqyay)@ay)", &version, &frames, &cal);
          g_variant_unref (cal);
        }
      else if (g_variant_is_of_type (fpi, G_VARIANT_TYPE ("(yaa(qqyay))")))
        {
          g_variant_get (fpi, "(y@aa(qqyay))", &version, &frames);
        }
      else
        continue;

      if ((g_variant_is_of_type (fpi, G_VARIANT_TYPE ("(yaa(qqyay)ay)")) && version != 2) ||
          (g_variant_is_of_type (fpi, G_VARIANT_TYPE ("(yaa(qqyay))")) && version != 1))
        continue;

      nf = (guint) g_variant_n_children (frames);
      if (nf == 0 || nf > MAX_GALLERY)
        continue;

      g_snprintf (s->label, sizeof (s->label), "storage:%s", key);
      for (guint fi = 0; fi < nf; fi++)
        {
          GVariant *frame = g_variant_get_child_value (frames, fi);
          guint nk = (guint) g_variant_n_children (frame);
          Egis0575MFeatureSet *fs = &s->enrolled[n];

          fs->n = 0;
          for (guint k = 0; k < nk && k < EGIS0575_M_MAX_FEATURES; k++)
            {
              GVariant *feat = g_variant_get_child_value (frame, k);
              GVariant *desc;
              Egis0575MFeature *ft = &fs->f[fs->n];
              gsize dlen = 0;
              const void *raw_desc;
              gint64 x, y;
              guint8 orient;

              g_variant_get (feat, "(qqy@ay)", &x, &y, &orient, &desc);
              raw_desc = g_variant_get_fixed_array (desc, &dlen, 1);
              if (dlen == EGIS0575_M_DESC_BYTES && orient < EGIS0575_M_N_ORIENT)
                {
                  ft->x = (uint16_t) x;
                  ft->y = (uint16_t) y;
                  ft->orient = orient;
                  memcpy (ft->desc, raw_desc, EGIS0575_M_DESC_BYTES);
                  fs->n++;
                }
              g_variant_unref (desc);
              g_variant_unref (feat);
            }
          g_variant_unref (frame);
          n++;
        }
      break;  /* one enrolled print per storage file in practice */
    }

  g_variant_unref (dict);
  return n;
}

/* ------------------------------------------------------------------- main */

int
main (int argc, char **argv)
{
  const char *datasets = "datasets";
  const char *storage = NULL;
  struct stat storage_st;

  for (int i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "--storage") == 0 && i + 1 < argc)
        storage = argv[++i];
      else
        datasets = argv[i];
    }

  /* ---- scan verify-run dirs ---- */
  DIR *dp = opendir (datasets);
  if (!dp)
    {
      fprintf (stderr, "cannot open %s\n", datasets);
      return 1;
    }

  struct dirent *de;
  while ((de = readdir (dp)) != NULL)
    {
      if (strncmp (de->d_name, "verify-run-", 11) != 0 || n_runs >= 256)
        continue;

      Run *r = &runs[n_runs];
      char path[512];
      struct stat st;

      g_snprintf (r->name, sizeof (r->name), "%s", de->d_name);
      r->session = -1;
      r->n_probes = 0;
      r->base_best = r->fb_best = 0;
      r->taught_this_run = 0;
      r->fb_match = 0;

      g_snprintf (path, sizeof (path), "%s/%s", datasets, de->d_name);
      if (stat (path, &st) != 0 || !S_ISDIR (st.st_mode))
        continue;
      r->mtime = (long long) st.st_mtime;

      /* probes (capture order = filename order) */
      for (int p = 0; p < MAX_PROBES; p++)
        {
          char ppath[640];
          int w, h;
          unsigned char *img;

          g_snprintf (ppath, sizeof (ppath), "%s/probe-%03d.pgm", path, p);
          img = load_pgm (ppath, &w, &h);
          if (!img)
            break;
          egis0575_m_extract (img, w, h, &r->probes[r->n_probes]);
          free (img);
          r->n_probes++;
        }

      /* gallery: hash the raw dumps to find the session */
      uint64_t h = 1469598103934665603ULL;
      int n_gal = 0;
      for (int g = 0; g < MAX_GALLERY; g++)
        {
          char gpath[640];

          g_snprintf (gpath, sizeof (gpath), "%s/gallery-%03d.pgm", path, g);
          uint64_t before = h;
          h = fnv1a_file (gpath, h);
          if (h == before)
            break;
          n_gal++;
        }
      if (n_gal == 0)
        {
          n_runs++;  /* gallery-less: may be assigned to --storage below */
          continue;
        }

      int si = -1;
      for (int i = 0; i < n_sessions; i++)
        if (sessions[i].hash == h)
          {
            si = i;
            break;
          }
      if (si < 0)
        {
          if (n_sessions >= 16)
            {
              fprintf (stderr, "too many sessions\n");
              return 1;
            }
          si = n_sessions++;
          Session *s = &sessions[si];
          s->hash = h;
          s->n_enrolled = 0;
          s->n_runs = 0;
          s->taught_total = 0;
          g_snprintf (s->label, sizeof (s->label), "gal-%s", r->name);

          for (int g = 0; g < n_gal; g++)
            {
              char gpath[640];
              int w, hh;
              unsigned char *img;

              g_snprintf (gpath, sizeof (gpath), "%s/gallery-%03d.pgm", path, g);
              img = load_pgm (gpath, &w, &hh);
              if (!img)
                break;
              egis0575_m_extract (img, w, hh, &s->enrolled[s->n_enrolled]);
              free (img);
              s->n_enrolled++;
            }
        }
      r->session = si;
      sessions[si].n_runs++;
      n_runs++;
    }
  closedir (dp);

  /* ---- storage template session for gallery-less runs ---- */
  int storage_session = -1;
  if (storage && stat (storage, &storage_st) == 0)
    {
      if (n_sessions < 16)
        {
          Session *s = &sessions[n_sessions];
          int n = load_storage_gallery (storage, s);

          if (n > 0)
            {
              s->hash = 0x53544f52414745ULL;  /* "STORAGE" */
              s->n_enrolled = n;
              s->n_runs = 0;
              s->taught_total = 0;
              storage_session = n_sessions++;
              for (int i = 0; i < n_runs; i++)
                if (runs[i].session < 0 && runs[i].n_probes > 0 &&
                    runs[i].mtime >= (long long) storage_st.st_mtime)
                  {
                    runs[i].session = storage_session;
                    s->n_runs++;
                  }
            }
        }
    }

  qsort (runs, n_runs, sizeof (Run), cmp_runs_by_mtime);

  /* ---- baseline pass + feedback simulation per session ---- */
  for (int si = 0; si < n_sessions; si++)
    {
      Session *s = &sessions[si];
      s->n_frames = s->n_enrolled;
      memcpy (s->frames, s->enrolled, s->n_enrolled * sizeof (Egis0575MFeatureSet));
    }

  for (int i = 0; i < n_runs; i++)
    {
      Run *r = &runs[i];
      Session *s;

      if (r->session < 0 || r->n_probes == 0)
        continue;
      s = &sessions[r->session];

      /* baseline: best score against the pristine enrolled gallery */
      for (int p = 0; p < r->n_probes; p++)
        {
          int best, agree;
          probe_match (&r->probes[p], s->enrolled, s->n_enrolled, &best, &agree);
          if (best > r->base_best)
            {
              r->base_best = best;
              r->base_agree_at_best = agree;
            }
        }

      /* feedback: replay probes in order with early exit, then teach */
      {
        int best_overall = 0;
        const Egis0575MFeatureSet *best_probe = NULL;
        int matched = 0;

        for (int p = 0; p < r->n_probes; p++)
          {
            int best, agree;
            int m = probe_match (&r->probes[p], s->frames, s->n_frames,
                                 &best, &agree);

            if (best > best_overall)
              {
                best_overall = best;
                best_probe = &r->probes[p];
              }
            if (m)
              {
                matched = 1;
                break;  /* early exit: later probes would not exist */
              }
          }
        r->fb_best = best_overall;
        r->fb_match = matched;
        if (matched && best_probe && best_overall >= TEACH_SCORE)
          if (teach (s, best_probe))
            {
              r->taught_this_run = 1;
              s->taught_total++;
            }
      }
    }

  /* ---- report ---- */
  int total_probes = 0, scored_runs = 0, unscorable = 0;
  for (int i = 0; i < n_runs; i++)
    {
      total_probes += runs[i].n_probes;
      if (runs[i].session >= 0 && runs[i].n_probes > 0)
        scored_runs++;
      else if (runs[i].n_probes > 0)
        unscorable++;
    }

  printf ("语料：%d 个 verify-run（%d 个可评分 / %d 个无画廊且无法归属 / "
          "%d 个 probe 总计），%d 个录入会话\n\n",
          n_runs, scored_runs, unscorable, total_probes, n_sessions);

  printf ("阈值：match %d（agree ≥%d 帧 × ≥%d），回授门 %d，去重门 %d，"
          "画廊上限 %d 帧\n\n",
          EGIS0575_M_MATCH_THRESHOLD, EGIS0575_M_AGREE_FRAMES,
          EGIS0575_M_AGREE_SCORE, TEACH_SCORE, DEDUP_GATE, GALLERY_MAX_FRAMES);

  int global_low_max_base = 0, global_low_max_fb = 0;  /* sub-threshold runs */
  int teach_exposure = 0;

  for (int si = 0; si < n_sessions; si++)
    {
      Session *s = &sessions[si];
      printf ("会话 %d：%s（%d 帧录入模板，%d 个运行）\n",
              si, s->label, s->n_enrolled, s->n_runs);
      printf ("  %-38s %4s %6s %6s %6s %5s\n",
              "运行", "探针", "基线", "回馈", "判定", "教入");
      for (int i = 0; i < n_runs; i++)
        {
          Run *r = &runs[i];
          const char *mark = "";

          if (r->session != si || r->n_probes == 0)
            continue;
          if (r->base_best >= 300 && r->base_best < 349)
            mark = "  <== 灰区";  /* below trusted-genuine floor 349 */
          printf ("  %-38s %4d %6d %6d %6s %5s%s\n",
                  r->name, r->n_probes, r->base_best, r->fb_best,
                  r->fb_match ? "MATCH" : "-",
                  r->taught_this_run ? "是" : "", mark);

          if (r->base_best < EGIS0575_M_MATCH_THRESHOLD)
            {
              if (r->base_best > global_low_max_base)
                global_low_max_base = r->base_best;
              if (r->fb_best > global_low_max_fb)
                global_low_max_fb = r->fb_best;
            }
          if (r->fb_best >= TEACH_SCORE && r->taught_this_run)
            teach_exposure++;
        }
      printf ("  → 回授教入 %d 帧（最终画廊 %d 帧）\n\n",
              s->taught_total, s->n_frames);
    }

  printf ("== 回馈安全性（本会话语料内）==\n");
  printf ("基线低于阈值 %d 的运行：基线峰值 %d，回馈后峰值 %d%s\n",
          EGIS0575_M_MATCH_THRESHOLD, global_low_max_base, global_low_max_fb,
          global_low_max_fb >= EGIS0575_M_MATCH_THRESHOLD
            ? "  ⚠ 回馈使低分运行越过阈值！" : "（未越阈，回馈 FAR 中性）");
  printf ("触发回授的运行数（%d 分门）：%d\n\n", TEACH_SCORE, teach_exposure);

  /* ---- cross-session matrix vs final adapted galleries ---- */
  printf ("== 跨会话矩阵（每 probe vs 他会话最终画廊；同指重录会"
          "抬高此值，非纯冒充集）==\n");
  int cross_max = 0;
  char cross_max_run[128] = "";
  int cross_max_gal = -1;

  for (int i = 0; i < n_runs; i++)
    {
      Run *r = &runs[i];
      if (r->session < 0 || r->n_probes == 0)
        continue;

      for (int sj = 0; sj < n_sessions; sj++)
        {
          int best, agree;

          if (sj == r->session)
            continue;
          for (int p = 0; p < r->n_probes; p++)
            {
              probe_match (&r->probes[p], sessions[sj].frames,
                           sessions[sj].n_frames, &best, &agree);
              if (best > cross_max)
                {
                  cross_max = best;
                  g_snprintf (cross_max_run, sizeof (cross_max_run), "%s", r->name);
                  cross_max_gal = sj;
                }
            }
        }
    }
  if (cross_max_gal >= 0)
    printf ("跨会话峰值 %d（%s vs 会话 %d 画廊；阈值 %d，回授门 %d）\n",
            cross_max, cross_max_run, cross_max_gal,
            EGIS0575_M_MATCH_THRESHOLD, TEACH_SCORE);
  else
    printf ("（不足两个会话，跳过）\n");

  return 0;
}
