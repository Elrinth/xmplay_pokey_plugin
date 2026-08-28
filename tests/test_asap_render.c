/*
 * Host-side render / seek / detect / mute / reject tests for xmp-pokey.
 */
#include "pokey_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static int g_fail;

static unsigned char *slurp(const char *path, size_t *out_len)
{
  FILE *f;
  unsigned char *buf;
  long sz;
  *out_len = 0;
  f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  sz = ftell(f);
  if (sz < 1) { fclose(f); return NULL; }
  rewind(f);
  buf = (unsigned char *)malloc((size_t)sz);
  if (!buf) { fclose(f); return NULL; }
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    free(buf); fclose(f); return NULL;
  }
  fclose(f);
  *out_len = (size_t)sz;
  return buf;
}

static void rms_peak(const float *s, int frames, double *rms, double *peak)
{
  double acc = 0.0, pk = 0.0;
  int i, n = frames * 2;
  for (i = 0; i < n; ++i) {
    double v = s[i];
    acc += v * v;
    if (v < 0) v = -v;
    if (v > pk) pk = v;
  }
  *rms = n > 0 ? sqrt(acc / n) : 0.0;
  *peak = pk;
}

static int render_sec(pokey_player *p, double sec, double *rms, double *peak)
{
  int rate = pokey_player_rate(p);
  int need = (int)(sec * rate + 0.5);
  int got = 0;
  float *buf;
  if (need < 64) need = 64;
  buf = (float *)calloc((size_t)need * 2, sizeof(float));
  if (!buf) return -1;
  while (got < need) {
    int n = pokey_player_process(p, buf + got * 2, (need - got) * 2);
    if (n <= 0) break;
    got += n / 2;
  }
  rms_peak(buf, got, rms, peak);
  free(buf);
  return got;
}

static int drain_until_end(pokey_player *p, int cap_ms)
{
  float buf[2048];
  int loops = 0;
  int last = 0;
  for (;;) {
    int n = pokey_player_process(p, buf, 2048);
    last = pokey_player_position_ms(p);
    if (n <= 0)
      return last;
    if (++loops > 100000)
      return -1;
    if (last > cap_ms + 2000)
      return -2;
  }
}

static int test_file(const char *path, int expect_songs_min, int expect_silent)
{
  unsigned char *data;
  size_t len;
  pokey_info inf;
  pokey_player *p;
  double rms, peak;
  int frames, i, pos0, pos1, seek_to;
  const char *base;

  base = strrchr(path, '/');
  base = base ? base + 1 : path;
  printf("==== %s ====\n", base);

  data = slurp(path, &len);
  if (!data) {
    printf("FAIL  cannot read\n");
    g_fail++;
    return -1;
  }
  printf("file: %zu bytes\n", len);

  if (!pokey_probe(path, data, len)) {
    printf("FAIL  probe rejected\n");
    g_fail++;
    free(data);
    return -1;
  }

  if (pokey_analyze(path, data, len, 2, &inf) != 0) {
    printf("FAIL  analyze\n");
    g_fail++;
    free(data);
    return -1;
  }
  printf("title: %s\n", inf.title);
  printf("author: %s\n", inf.author);
  printf("songs: %d  default: %d  ch: %d  orig: %s\n",
         inf.songs, inf.default_song + 1, inf.channels, inf.orig_ext);
  for (i = 0; i < inf.songs && i < 8; ++i) {
    printf("  [%d] one_loop=%d ms  play=%d ms  loop=%d  detected=%d\n",
           i + 1, inf.one_loop_ms[i], inf.play_ms[i],
           inf.loops[i], inf.detected[i]);
  }
  if (inf.songs < expect_songs_min) {
    printf("FAIL  expected at least %d songs\n", expect_songs_min);
    g_fail++;
  }

  p = pokey_player_open(path, data, len, 2, 0);
  free(data);
  if (!p) {
    printf("FAIL  open\n");
    g_fail++;
    return -1;
  }

  frames = render_sec(p, 2.0, &rms, &peak);
  printf("render 2.0s: frames=%d  rms=%.5f  peak=%.5f\n", frames, rms, peak);
  if (!expect_silent && rms < 1e-4) {
    printf("FAIL  silent render\n");
    g_fail++;
  }

  pos0 = pokey_player_position_ms(p);
  seek_to = inf.play_ms[0] / 2;
  if (seek_to < 1000) seek_to = 1000;
  if (seek_to > 8000) seek_to = 8000;
  if (inf.play_ms[0] > 0 && seek_to > inf.play_ms[0] * 8 / 10)
    seek_to = inf.play_ms[0] / 2;
  pos1 = pokey_player_seek_ms(p, seek_to);
  printf("seek to %d ms -> %d ms (was %d)\n", seek_to, pos1, pos0);
  if (pos1 < 0 || pos1 < seek_to / 2) {
    printf("FAIL  seek did not advance\n");
    g_fail++;
  }
  frames = render_sec(p, 0.4, &rms, &peak);
  printf("post-seek 0.4s: frames=%d  rms=%.5f  peak=%.5f  tell=%d\n",
         frames, rms, peak, pokey_player_position_ms(p));

  if (inf.songs > 1) {
    if (pokey_player_set_song(p, 1) != 0) {
      printf("FAIL  switch track\n");
      g_fail++;
    } else {
      frames = render_sec(p, 0.5, &rms, &peak);
      printf("track 2: frames=%d  rms=%.5f  peak=%.5f  play=%d ms\n",
             frames, rms, peak, pokey_player_play_ms(p, 1));
      if (rms < 1e-4) {
        printf("FAIL  silent track 2\n");
        g_fail++;
      }
    }
  }

  pokey_player_close(p);
  return 0;
}

static int test_detect_unknown(const char *with_time, const char *no_time)
{
  unsigned char *d1, *d0;
  size_t n1, n0;
  pokey_info a1, a0;
  pokey_player *p;
  int end_ms;

  printf("==== detect: TIME vs stripped ====\n");
  d1 = slurp(with_time, &n1);
  d0 = slurp(no_time, &n0);
  if (!d1 || !d0) {
    printf("FAIL  cannot read detect pair\n");
    g_fail++;
    free(d1); free(d0);
    return -1;
  }
  if (pokey_analyze(with_time, d1, n1, 2, &a1) != 0) {
    printf("FAIL  analyze with TIME\n");
    g_fail++;
    free(d1); free(d0);
    return -1;
  }
  printf("WITH TIME:  one_loop=%d ms  play=%d ms  loop=%d  detected=%d  title=%s\n",
         a1.one_loop_ms[0], a1.play_ms[0], a1.loops[0], a1.detected[0], a1.title);
  if (a1.detected[0] || a1.one_loop_ms[0] < 0) {
    printf("FAIL  expected tagged duration\n");
    g_fail++;
  }

  if (pokey_analyze(no_time, d0, n0, 2, &a0) != 0) {
    printf("FAIL  analyze without TIME\n");
    g_fail++;
    free(d1); free(d0);
    return -1;
  }
  printf("NO TIME:    one_loop=%d ms  play=%d ms  loop=%d  detected=%d  title=%s\n",
         a0.one_loop_ms[0], a0.play_ms[0], a0.loops[0], a0.detected[0], a0.title);
  printf("per-sample duration before/after detect: %d ms -> %d ms\n",
         a1.one_loop_ms[0], a0.one_loop_ms[0]);
  if (!a0.detected[0]) {
    printf("FAIL  expected silence-detect path\n");
    g_fail++;
  }
  if (a0.one_loop_ms[0] >= POKEY_DETECT_CAP_MS - 50) {
    printf("NOTE  detect hit 10-minute cap (song never silenced)\n");
  } else if (a0.one_loop_ms[0] < 500) {
    printf("FAIL  detected length implausibly short (%d ms)\n", a0.one_loop_ms[0]);
    g_fail++;
  }

  /* PlaySong with detected duration must end (Generate returns 0). */
  p = pokey_player_open(no_time, d0, n0, 1, 0); /* 1 loop: play == one_loop */
  free(d1);
  free(d0);
  if (!p) {
    printf("FAIL  open no-TIME\n");
    g_fail++;
    return -1;
  }
  printf("opened no-TIME play_ms=%d (must not be PlaySong -1)\n",
         pokey_player_play_ms(p, 0));
  end_ms = drain_until_end(p, pokey_player_play_ms(p, 0));
  printf("Generate returned 0 at %d ms (play_ms=%d)\n",
         end_ms, pokey_player_play_ms(p, 0));
  if (end_ms < 0) {
    printf("FAIL  playback did not end\n");
    g_fail++;
  }
  pokey_player_close(p);
  return 0;
}

static int test_mute(const char *path)
{
  unsigned char *data;
  size_t len;
  pokey_player *p;
  double rms, peak;
  int frames;

  printf("==== mute mask ====\n");
  data = slurp(path, &len);
  if (!data) { printf("FAIL  mute slurp\n"); g_fail++; return -1; }
  p = pokey_player_open(path, data, len, 2, 0xFF);
  free(data);
  if (!p) { printf("FAIL  mute open\n"); g_fail++; return -1; }
  frames = render_sec(p, 1.0, &rms, &peak);
  printf("mute all 8: frames=%d  rms=%.6f  peak=%.6f\n", frames, rms, peak);
  if (rms > 1e-4 || peak > 1e-3) {
    printf("FAIL  muted render not silent\n");
    g_fail++;
  }
  pokey_player_set_mute(p, 0);
  pokey_player_seek_ms(p, 0);
  frames = render_sec(p, 1.0, &rms, &peak);
  printf("unmute:     frames=%d  rms=%.6f  peak=%.6f\n", frames, rms, peak);
  if (rms < 1e-4) {
    printf("FAIL  unmuted render silent\n");
    g_fail++;
  }
  pokey_player_close(p);
  return 0;
}

static int test_reject(const char *path)
{
  unsigned char *data;
  size_t len;
  int ok;
  printf("==== reject %s ====\n", path);
  data = slurp(path, &len);
  if (!data) { printf("FAIL  cannot read reject fixture\n"); g_fail++; return -1; }
  ok = pokey_probe(path, data, len);
  free(data);
  if (ok) {
    printf("FAIL  non-ASAP file was accepted\n");
    g_fail++;
    return -1;
  }
  printf("rejected (Load failed) — good\n");
  return 0;
}

int main(int argc, char **argv)
{
  const char *root = "tests/samples";
  char p_spy[256], p_spy0[256], p_fru[256], p_fru0[256];
  char p_heb[256], p_fc[256], p_amiga[256], p_dmf[256], p_vee[256];
  (void)argc; (void)argv;

  snprintf(p_spy, sizeof p_spy, "%s/Spy_vs_Spy.sap", root);
  snprintf(p_spy0, sizeof p_spy0, "%s/Spy_vs_Spy_notime.sap", root);
  snprintf(p_fru, sizeof p_fru, "%s/Fruity_Pete_Game_Over.sap", root);
  snprintf(p_fru0, sizeof p_fru0, "%s/Fruity_Pete_notime.sap", root);
  snprintf(p_heb, sizeof p_heb, "%s/Hebdzie_1.sap", root);
  snprintf(p_fc, sizeof p_fc, "%s/NINJA.FC", root);
  snprintf(p_amiga, sizeof p_amiga, "%s/amiga_dummy.fc", root);
  snprintf(p_dmf, sizeof p_dmf, "%s/dummy.dmf", root);
  snprintf(p_vee, sizeof p_vee, "%s/Veeblefetzer.sap", root);

  printf("xmp-pokey host tests  loop_count default=%d\n", POKEY_DEFAULT_LOOPS);

  test_file(p_spy, 1, 0);
  test_file(p_heb, 2, 0);
  test_file(p_fc, 1, 0);

  /* Tagged vs silence-detect. Fruity Pete is a short one-shot (no LOOP). */
  test_detect_unknown(p_fru, p_fru0);
  /* Spy vs Spy historically had no TIME; ASMA now tags 00:19.25 LOOP.
     Stripped copy proves the detect path still runs. */
  test_detect_unknown(p_spy, p_spy0);

  test_mute(p_spy);
  test_reject(p_amiga);
  test_reject(p_dmf);

  /* genuine ASMA file without TIME */
  {
    unsigned char *d; size_t n; pokey_info inf;
    d = slurp(p_vee, &n);
    if (d && pokey_analyze(p_vee, d, n, 2, &inf) == 0) {
      printf("==== Veeblefetzer (no TIME in ASMA) ====\n");
      printf("one_loop=%d ms  play=%d ms  detected=%d  title=%s\n",
             inf.one_loop_ms[0], inf.play_ms[0], inf.detected[0], inf.title);
      if (inf.one_loop_ms[0] >= POKEY_DETECT_CAP_MS - 50)
        printf("NOTE  never silenced — 10 min cap\n");
    } else {
      printf("FAIL  Veeblefetzer\n");
      g_fail++;
    }
    free(d);
  }

  printf("==== %s  fails=%d ====\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
