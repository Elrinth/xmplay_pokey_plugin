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

/* Insert a TIME line immediately after "SAP\r\n". */
static unsigned char *insert_time_tag(const unsigned char *src, size_t n,
                                      const char *time_line, size_t *out_n)
{
  const char sap[] = "SAP\r\n";
  size_t tlen;
  unsigned char *out;
  if (!src || n < 5 || memcmp(src, sap, 5) != 0 || !time_line)
    return NULL;
  tlen = strlen(time_line);
  out = (unsigned char *)malloc(n + tlen);
  if (!out) return NULL;
  memcpy(out, src, 5);
  memcpy(out + 5, time_line, tlen);
  memcpy(out + 5 + tlen, src + 5, n - 5);
  *out_n = n + tlen;
  return out;
}

static void dump_file(const char *path)
{
  unsigned char *data;
  size_t len;
  pokey_info inf;
  const char *base;
  int i;

  base = strrchr(path, '/');
  base = base ? base + 1 : path;
  data = slurp(path, &len);
  if (!data) {
    printf("DUMP  %-28s  cannot read\n", base);
    return;
  }
  if (pokey_analyze(path, data, len, 1, &inf) != 0) {
    printf("DUMP  %-28s  analyze FAIL (%zu bytes)\n", base, len);
    free(data);
    return;
  }
  for (i = 0; i < inf.songs; ++i) {
    printf("DUMP  %-28s  song %d  tagged=%d  LOOP=%d  analyzed=%d  detected=%d%s\n",
           base, i, inf.tagged_ms[i], inf.loops[i],
           inf.one_loop_ms[i], inf.detected[i],
           pokey_is_dummy_time(inf.tagged_ms[i]) ? "  DUMMY3:00" : "");
  }
  free(data);
}

static int test_dummy_180000(const char *no_time_path)
{
  unsigned char *raw, *stub;
  size_t nraw, nstub;
  pokey_info inf0, inf3;

  printf("==== dummy TIME 03:00 must not stay 180000 ====\n");
  raw = slurp(no_time_path, &nraw);
  if (!raw) {
    printf("FAIL  cannot read no-TIME fixture\n");
    g_fail++;
    return -1;
  }
  if (pokey_analyze(no_time_path, raw, nraw, 1, &inf0) != 0) {
    printf("FAIL  analyze no-TIME\n");
    g_fail++;
    free(raw);
    return -1;
  }
  stub = insert_time_tag(raw, nraw, "TIME 03:00\r\n", &nstub);
  free(raw);
  if (!stub) {
    printf("FAIL  insert TIME 03:00\n");
    g_fail++;
    return -1;
  }
  if (pokey_analyze("dummy3.sap", stub, nstub, 1, &inf3) != 0) {
    printf("FAIL  analyze dummy 03:00\n");
    g_fail++;
    free(stub);
    return -1;
  }
  free(stub);
  printf("NO TIME:     tagged=%d  analyzed=%d  detected=%d\n",
         inf0.tagged_ms[0], inf0.one_loop_ms[0], inf0.detected[0]);
  printf("TIME 03:00:  tagged=%d  analyzed=%d  detected=%d\n",
         inf3.tagged_ms[0], inf3.one_loop_ms[0], inf3.detected[0]);
  if (inf3.tagged_ms[0] != 180000) {
    printf("FAIL  injected TIME 03:00 did not parse as 180000 (got %d)\n",
           inf3.tagged_ms[0]);
    g_fail++;
  }
  if (inf3.one_loop_ms[0] == 180000 && inf3.detected[0] == 0) {
    printf("FAIL  dummy 180000 was trusted without detect\n");
    g_fail++;
  }
  /* Unless detect itself lands on ~180000 (true 3-minute tune), analyzed
   * must not stay at the stub. Fruity Pete is ~6s. */
  if (inf0.one_loop_ms[0] < POKEY_DETECT_CAP_MS - 50) {
    if (inf3.one_loop_ms[0] == 180000) {
      printf("FAIL  dummy 180000 survived analyze (detect found %d on no-TIME)\n",
             inf0.one_loop_ms[0]);
      g_fail++;
    }
    if (!inf3.detected[0]) {
      printf("FAIL  dummy 03:00 should use silence-detect\n");
      g_fail++;
    }
    if (inf3.one_loop_ms[0] < 500) {
      printf("FAIL  dummy-path length implausibly short (%d)\n",
             inf3.one_loop_ms[0]);
      g_fail++;
    }
  }
  return 0;
}

static int test_spy_keeps_19250(const char *spy_path)
{
  unsigned char *d;
  size_t n;
  pokey_info inf;

  printf("==== Spy vs Spy tagged 19250 must stay 19250 ====\n");
  d = slurp(spy_path, &n);
  if (!d || pokey_analyze(spy_path, d, n, 1, &inf) != 0) {
    printf("FAIL  spy analyze\n");
    g_fail++;
    free(d);
    return -1;
  }
  free(d);
  printf("tagged=%d  analyzed=%d  detected=%d  LOOP=%d\n",
         inf.tagged_ms[0], inf.one_loop_ms[0], inf.detected[0], inf.loops[0]);
  if (inf.tagged_ms[0] != 19250) {
    printf("FAIL  expected ASAP GetDuration 19250, got %d\n", inf.tagged_ms[0]);
    g_fail++;
  }
  if (inf.one_loop_ms[0] != 19250 || inf.detected[0]) {
    printf("FAIL  real TIME 19250 was re-detected or changed (one_loop=%d detected=%d)\n",
           inf.one_loop_ms[0], inf.detected[0]);
    g_fail++;
  }
  return 0;
}


/* Greg / Bomb Song: last COM block is 2 bytes short; wrapper must pad. */
static int test_bomb_song(const char *path)
{
  unsigned char *data;
  size_t len;
  pokey_info inf;
  pokey_player *p;
  double rms, peak;
  int frames, i, idx, start, end, block_len, player_inside;

  printf("==== Bomb Song truncated COM ====\n");
  data = slurp(path, &len);
  if (!data) {
    printf("FAIL  cannot read bomb-song.sap\n");
    g_fail++;
    return -1;
  }
  printf("file: %zu bytes\n", len);

  /* Dump COM blocks: PLAYER $0503 must sit in the INIT $04F3 block. */
  idx = -1;
  for (i = 5; i + 1 < (int)len; ++i) {
    if (data[i] == 255 && data[i + 1] == 255) { idx = i + 2; break; }
  }
  player_inside = 0;
  i = 0;
  while (idx >= 0 && idx + 5 <= (int)len) {
    start = data[idx] | (data[idx + 1] << 8);
    end = data[idx + 2] | (data[idx + 3] << 8);
    block_len = end + 1 - start;
    printf("  COM[%d] $%04X-$%04X  claimed=%d  remain_after_hdr=%d\n",
           i, start, end, block_len, (int)len - (idx + 4));
    if (start <= 0x0503 && 0x0503 <= end)
      player_inside = 1;
    if (block_len <= 0)
      break;
    idx += 4 + block_len;
    if (idx == (int)len)
      break;
    if (idx + 1 < (int)len && data[idx] == 255 && data[idx + 1] == 255)
      idx += 2;
    if (++i > 8)
      break;
  }
  printf("PLAYER $0503 inside INIT $04F3 block: %s\n",
         player_inside ? "yes" : "NO");

  if (!pokey_probe(path, data, len)) {
    printf("FAIL  pokey_probe rejected bomb-song.sap\n");
    g_fail++;
    free(data);
    return -1;
  }
  if (pokey_analyze(path, data, len, 1, &inf) != 0) {
    printf("FAIL  pokey_analyze bomb-song.sap\n");
    g_fail++;
    free(data);
    return -1;
  }
  printf("title: %s  author: %s  one_loop=%d detected=%d\n",
         inf.title, inf.author, inf.one_loop_ms[0], inf.detected[0]);
  if (!strstr(inf.title, "Bomb")) {
    printf("FAIL  title does not contain Bomb (got '%s')\n", inf.title);
    g_fail++;
  }

  p = pokey_player_open(path, data, len, 1, 0);
  free(data);
  if (!p) {
    printf("FAIL  pokey_player_open bomb-song.sap\n");
    g_fail++;
    return -1;
  }
  frames = render_sec(p, 2.0, &rms, &peak);
  printf("render 2.0s: frames=%d  rms=%.5f  peak=%.5f\n", frames, rms, peak);
  if (rms < 1e-4 || peak < 1e-4) {
    printf("FAIL  Bomb Song render is silent\n");
    g_fail++;
  }
  pokey_player_close(p);
  return 0;
}

int main(int argc, char **argv)
{
  const char *root = "tests/samples";
  char p_spy[256], p_spy0[256], p_fru[256], p_fru0[256];
  char p_heb[256], p_fc[256], p_amiga[256], p_dmf[256], p_vee[256], p_bomb[256];
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
  snprintf(p_bomb, sizeof p_bomb, "%s/bomb-song.sap", root);

  printf("xmp-pokey host tests  loop_count default=%d\n", POKEY_DEFAULT_LOOPS);
  if (POKEY_DEFAULT_LOOPS != 1) {
    printf("FAIL  default loops is %d, expected 1\n", POKEY_DEFAULT_LOOPS);
    g_fail++;
  }

  /* Default loops=1: looping TIME play_ms must equal native one_loop_ms. */
  {
    unsigned char *d; size_t n; pokey_info inf;
    d = slurp(p_spy, &n);
    if (!d || pokey_analyze(p_spy, d, n, POKEY_DEFAULT_LOOPS, &inf) != 0) {
      printf("FAIL  default-loop analyze\n");
      g_fail++;
    } else {
      printf("==== default loops=%d  Spy vs Spy one_loop=%d play=%d loop=%d ====\n",
             POKEY_DEFAULT_LOOPS, inf.one_loop_ms[0], inf.play_ms[0], inf.loops[0]);
      if (inf.loops[0] && inf.play_ms[0] != inf.one_loop_ms[0]) {
        printf("FAIL  default loops should not multiply playlist/native length\n");
        g_fail++;
      }
    }
    free(d);
  }

  printf("==== host dump: GetDuration vs analyze ====\n");
  dump_file(p_spy);
  dump_file(p_spy0);
  dump_file(p_fru);
  dump_file(p_fru0);
  dump_file(p_heb);
  dump_file(p_fc);
  dump_file(p_vee);
  dump_file(p_bomb);
  {
    char extra[256];
    snprintf(extra, sizeof extra, "%s/Lasermania.sap", root);
    dump_file(extra);
    snprintf(extra, sizeof extra, "%s/Lasermania.cmc", root);
    dump_file(extra);
    snprintf(extra, sizeof extra, "%s/aurora_s.rmt", root);
    dump_file(extra);
  }

  test_spy_keeps_19250(p_spy);
  test_dummy_180000(p_fru0);

  test_file(p_spy, 1, 0);
  test_file(p_heb, 2, 0);
  test_file(p_fc, 1, 0);
  test_bomb_song(p_bomb);

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
