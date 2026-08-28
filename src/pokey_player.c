/*
 * ASAP engine wrapper for xmp-pokey.
 * Never calls ASAP_PlaySong(..., -1).
 */
#include "pokey_player.h"
#include "pokey_scan.h"
#include "asap.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct pokey_player {
  ASAP *asap;
  unsigned char *module;
  int module_len;
  char filename[512];
  int song;
  int songs;
  int channels;
  int rate;
  int loop_count;
  int mute_mask;
  int one_loop_ms[POKEY_MAX_SONGS];
  int play_ms[POKEY_MAX_SONGS];
  int loops[POKEY_MAX_SONGS];
  int detected[POKEY_MAX_SONGS];
  char title[POKEY_STR];
  char author[POKEY_STR];
  char date[POKEY_STR];
  char orig_ext[16];
  int ntsc;
  unsigned char scratch[65536];
};

static void bounded_copy(char *dst, size_t cap, const char *src)
{
  size_t n;
  if (!dst || cap == 0)
    return;
  if (!src) { dst[0] = '\0'; return; }
  n = strlen(src);
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static const char *usable_name(const char *filename)
{
  if (filename && filename[0])
    return filename;
  return NULL;
}


/*
 * ASAP_Load refuses some ripped TYPE B SAPs whose last Atari COM block
 * is a few bytes short of the end address (ASAP's length check runs
 * before skipping the 4-byte start/end, so memcpy overruns and the
 * final moduleIndex != moduleLen). Pad 1-16 missing tail bytes with
 * zeros and retry. Missing FFFF between COM blocks is already OK.
 */
#define POKEY_COM_PAD_MAX 16

static int pokey_get_word(const unsigned char *p)
{
  return (int)p[0] | ((int)p[1] << 8);
}

/* Offset of the SAP header's FF FF marker, or -1. */
static int sap_header_ffff(const unsigned char *data, int len)
{
  int i;
  if (len < 30 || data[0] != 'S' || data[1] != 'A' || data[2] != 'P')
    return -1;
  if (data[3] != '\r' || data[4] != '\n')
    return -1;
  for (i = 5; i + 1 < len; ++i) {
    if (data[i] == 255) {
      if (data[i + 1] != 255)
        return -1;
      return i;
    }
  }
  return -1;
}

/*
 * If the last COM block is 1-16 bytes short of its claimed end address,
 * return a malloc'd copy padded with zeros. Caller frees. NULL = no pad.
 */
static unsigned char *pad_truncated_com(const unsigned char *data, int len,
                                        int *out_len)
{
  int header, idx, start, end, block_len, remain, shortfall, padded_len;
  unsigned char *out;

  header = sap_header_ffff(data, len);
  if (header < 0)
    return NULL;
  idx = header + 2;
  while (idx + 5 <= len) {
    start = pokey_get_word(data + idx);
    end = pokey_get_word(data + idx + 2);
    block_len = end + 1 - start;
    if (block_len <= 0)
      return NULL;
    remain = len - (idx + 4);
    if (remain < block_len) {
      shortfall = block_len - remain;
      if (shortfall < 1 || shortfall > POKEY_COM_PAD_MAX)
        return NULL;
      padded_len = len + shortfall;
      if (padded_len > ASAPInfo_MAX_MODULE_LENGTH)
        return NULL;
      out = (unsigned char *)malloc((size_t)padded_len);
      if (!out)
        return NULL;
      memcpy(out, data, (size_t)len);
      memset(out + len, 0, (size_t)shortfall);
      *out_len = padded_len;
      return out;
    }
    idx += 4 + block_len;
    if (idx == len)
      return NULL;
    if (idx + 7 <= len && data[idx] == 255 && data[idx + 1] == 255)
      idx += 2;
  }
  return NULL;
}

static int pokey_asap_load(ASAP *a, const char *filename,
                           const unsigned char *data, int len)
{
  unsigned char *padded;
  int plen, ok;

  if (!a || !data || len < 4)
    return 0;
  if (ASAP_Load(a, filename, data, len))
    return 1;
  padded = pad_truncated_com(data, len, &plen);
  if (!padded)
    return 0;
  ok = ASAP_Load(a, filename, padded, plen) ? 1 : 0;
  free(padded);
  return ok;
}


int pokey_play_ms_from(int one_loop_ms, int loops_flag, int loop_count, int unknown)
{
  int n, play;
  if (one_loop_ms < 0)
    one_loop_ms = 0;
  if (loop_count < 1) loop_count = 1;
  if (loop_count > 3) loop_count = 3;
  if (unknown)
    n = loop_count;
  else
    n = loops_flag ? loop_count : 1;
  play = one_loop_ms * n;
  if (play < 1)
    play = 1;
  return play;
}

int pokey_is_dummy_time(int ms)
{
  return ms >= POKEY_DUMMY_TIME_LO && ms <= POKEY_DUMMY_TIME_HI;
}

/*
 * asapscan-style silence + POKEY-register loop detect.
 * Never pass -1 to PlaySong. Cap 10 minutes only if neither is found.
 */
static int detect_one_loop_ms(const char *filename, const unsigned char *data,
                              int len, int song, int *out_loop)
{
  ASAP *a;
  int ms, is_loop, found;

  if (out_loop) *out_loop = 0;
  a = ASAP_New();
  if (!a)
    return -1;
  if (!pokey_asap_load(a, usable_name(filename), data, len)) {
    ASAP_Delete(a);
    return -1;
  }
  if (!ASAP_PlaySong(a, song, POKEY_DETECT_CAP_MS)) {
    ASAP_Delete(a);
    return -1;
  }
  if (pokey_scan_from_asap(a, &ms, &is_loop, &found) != 0) {
    ASAP_Delete(a);
    return -1;
  }
  ASAP_Delete(a);
  if (out_loop) *out_loop = is_loop;
  if (ms < 0)
    ms = 0;
  return ms;
}

/*
 * Always compute one-loop length:
 *   tagged < 0 (unknown TIME)           -> register loop / silence detect
 *   tagged in 179000-181000 (3:00 stub) -> same detect (dummy is not a length)
 *   any other tagged >= 0               -> keep TIME (real native length)
 * Detected loop sets *out_loop = 1. Silence or cap sets 0.
 * Only if neither silence nor loop: one_loop = 10-minute cap.
 */
static int resolve_one_loop_ms(const char *filename, const unsigned char *data,
                               int len, int song, int tagged, int *used_detect,
                               int *out_loop)
{
  int pos;

  if (used_detect) *used_detect = 0;
  if (out_loop) *out_loop = 0;
  if (tagged >= 0 && !pokey_is_dummy_time(tagged))
    return tagged;

  pos = detect_one_loop_ms(filename, data, len, song, out_loop);
  if (pos < 0)
    return -1;

  if (used_detect) *used_detect = 1;
  return pos;
}

static void fill_info_from_asap(const ASAP *a, const unsigned char *data, int len,
                                pokey_info *out)
{
  const ASAPInfo *info = ASAP_GetInfo(a);
  const char *ext;
  int i, n;

  memset(out, 0, sizeof *out);
  n = ASAPInfo_GetSongs(info);
  if (n < 1) n = 1;
  if (n > POKEY_MAX_SONGS) n = POKEY_MAX_SONGS;
  out->songs = n;
  out->default_song = ASAPInfo_GetDefaultSong(info);
  out->channels = ASAPInfo_GetChannels(info);
  if (out->channels < 1) out->channels = 1;
  if (out->channels > 2) out->channels = 2;
  out->ntsc = ASAPInfo_IsNtsc(info) ? 1 : 0;
  out->rate_hz = ASAP_GetSampleRate(a);
  bounded_copy(out->title, sizeof out->title, ASAPInfo_GetTitle(info));
  bounded_copy(out->author, sizeof out->author, ASAPInfo_GetAuthor(info));
  bounded_copy(out->date, sizeof out->date, ASAPInfo_GetDate(info));
  ext = ASAPInfo_GetOriginalModuleExt(info, data, len);
  bounded_copy(out->orig_ext, sizeof out->orig_ext, ext ? ext : "");
  for (i = 0; i < n; ++i)
    out->loops[i] = ASAPInfo_GetLoop(info, i) ? 1 : 0;
}

int pokey_probe(const char *filename, const unsigned char *data, size_t len)
{
  ASAP *a;
  int ok;
  if (!data || len < 4 || len > (size_t)ASAPInfo_MAX_MODULE_LENGTH)
    return 0;
  a = ASAP_New();
  if (!a)
    return 0;
  ok = pokey_asap_load(a, usable_name(filename), data, (int)len) ? 1 : 0;
  ASAP_Delete(a);
  return ok;
}

int pokey_analyze(const char *filename, const unsigned char *data, size_t len,
                  int loop_count, pokey_info *out)
{
  ASAP *a;
  const ASAPInfo *info;
  int i;

  if (!out)
    return -1;
  memset(out, 0, sizeof *out);
  if (!data || len < 4 || len > (size_t)ASAPInfo_MAX_MODULE_LENGTH)
    return -1;
  if (loop_count < 1) loop_count = 1;
  if (loop_count > 3) loop_count = 3;

  a = ASAP_New();
  if (!a)
    return -1;
  if (!pokey_asap_load(a, usable_name(filename), data, (int)len)) {
    ASAP_Delete(a);
    return -1;
  }
  fill_info_from_asap(a, data, (int)len, out);
  info = ASAP_GetInfo(a);
  for (i = 0; i < out->songs; ++i) {
    int tagged = ASAPInfo_GetDuration(info, i);
    int det = 0;
    int is_loop = 0;
    out->tagged_ms[i] = tagged;
    out->one_loop_ms[i] = resolve_one_loop_ms(filename, data, (int)len, i,
                                              tagged, &det, &is_loop);
    out->detected[i] = det;
    if (det)
      out->loops[i] = is_loop;
    if (out->one_loop_ms[i] < 0) {
      ASAP_Delete(a);
      return -1;
    }
    out->play_ms[i] = pokey_play_ms_from(out->one_loop_ms[i], out->loops[i],
                                         loop_count, out->detected[i]);
  }
  ASAP_Delete(a);
  return 0;
}

static int apply_play(pokey_player *p, int song)
{
  int play;
  if (!p || !p->asap)
    return -1;
  if (song < 0 || song >= p->songs)
    return -1;
  play = p->play_ms[song];
  if (play < 1)
    play = 1;
  /* NEVER pass -1 */
  if (!ASAP_PlaySong(p->asap, song, play))
    return -1;
  ASAP_MutePokeyChannels(p->asap, p->mute_mask);
  p->song = song;
  return 0;
}

static int recompute_play(pokey_player *p)
{
  int i;
  if (!p)
    return -1;
  for (i = 0; i < p->songs; ++i) {
    p->play_ms[i] = pokey_play_ms_from(p->one_loop_ms[i], p->loops[i],
                                       p->loop_count, p->detected[i]);
  }
  return 0;
}

pokey_player *pokey_player_open(const char *filename, const unsigned char *data,
                                size_t len, int loop_count, int mute_mask)
{
  pokey_player *p;
  pokey_info inf;
  int def;

  if (!data || len < 4 || len > (size_t)ASAPInfo_MAX_MODULE_LENGTH)
    return NULL;
  if (loop_count < 1) loop_count = 1;
  if (loop_count > 3) loop_count = 3;
  mute_mask &= 255;

  if (pokey_analyze(filename, data, len, loop_count, &inf) != 0)
    return NULL;

  p = (pokey_player *)calloc(1, sizeof *p);
  if (!p)
    return NULL;
  p->module = (unsigned char *)malloc(len);
  if (!p->module) {
    free(p);
    return NULL;
  }
  memcpy(p->module, data, len);
  p->module_len = (int)len;
  bounded_copy(p->filename, sizeof p->filename, filename ? filename : "");
  p->songs = inf.songs;
  p->channels = inf.channels;
  p->rate = inf.rate_hz > 0 ? inf.rate_hz : ASAP_SAMPLE_RATE;
  p->loop_count = loop_count;
  p->mute_mask = mute_mask;
  memcpy(p->one_loop_ms, inf.one_loop_ms, sizeof p->one_loop_ms);
  memcpy(p->play_ms, inf.play_ms, sizeof p->play_ms);
  memcpy(p->loops, inf.loops, sizeof p->loops);
  memcpy(p->detected, inf.detected, sizeof p->detected);
  memcpy(p->title, inf.title, sizeof p->title);
  memcpy(p->author, inf.author, sizeof p->author);
  memcpy(p->date, inf.date, sizeof p->date);
  memcpy(p->orig_ext, inf.orig_ext, sizeof p->orig_ext);
  p->ntsc = inf.ntsc;

  p->asap = ASAP_New();
  if (!p->asap || !pokey_asap_load(p->asap, usable_name(filename), p->module, p->module_len)) {
    pokey_player_close(p);
    return NULL;
  }
  def = inf.default_song;
  if (def < 0 || def >= p->songs)
    def = 0;
  if (apply_play(p, def) != 0) {
    pokey_player_close(p);
    return NULL;
  }
  return p;
}

void pokey_player_close(pokey_player *p)
{
  if (!p)
    return;
  if (p->asap) {
    ASAP_Delete(p->asap);
    p->asap = NULL;
  }
  free(p->module);
  free(p);
}

int pokey_player_songs(const pokey_player *p) { return p ? p->songs : 0; }
int pokey_player_song(const pokey_player *p) { return p ? p->song : 0; }
int pokey_player_channels(const pokey_player *p) { return p ? p->channels : 2; }
int pokey_player_rate(const pokey_player *p) { return p ? p->rate : ASAP_SAMPLE_RATE; }

int pokey_player_set_song(pokey_player *p, int song0)
{
  if (!p)
    return -1;
  return apply_play(p, song0);
}

int pokey_player_one_loop_ms(const pokey_player *p, int song0)
{
  if (!p || song0 < 0 || song0 >= p->songs)
    return 0;
  return p->one_loop_ms[song0];
}

int pokey_player_play_ms(const pokey_player *p, int song0)
{
  if (!p || song0 < 0 || song0 >= p->songs)
    return 0;
  return p->play_ms[song0];
}

int pokey_player_detected(const pokey_player *p, int song0)
{
  if (!p || song0 < 0 || song0 >= p->songs)
    return 0;
  return p->detected[song0];
}

int pokey_player_was_loop(const pokey_player *p, int song0)
{
  if (!p || song0 < 0 || song0 >= p->songs)
    return 0;
  return p->loops[song0];
}

int pokey_player_position_ms(const pokey_player *p)
{
  if (!p || !p->asap)
    return 0;
  return ASAP_GetPosition(p->asap);
}

int pokey_player_seek_ms(pokey_player *p, int ms)
{
  if (!p || !p->asap)
    return -1;
  if (ms < 0)
    ms = 0;
  if (ms > p->play_ms[p->song])
    ms = p->play_ms[p->song];
  if (!ASAP_Seek(p->asap, ms))
    return -1;
  /* Seek may restart via PlaySong, which unmutes — re-apply. */
  ASAP_MutePokeyChannels(p->asap, p->mute_mask);
  return ASAP_GetPosition(p->asap);
}

int pokey_player_total_play_ms(const pokey_player *p)
{
  int i, t = 0;
  if (!p)
    return 0;
  for (i = 0; i < p->songs; ++i)
    t += p->play_ms[i];
  return t;
}

int pokey_player_total_one_loop_ms(const pokey_player *p)
{
  int i, t = 0;
  if (!p)
    return 0;
  for (i = 0; i < p->songs; ++i)
    t += p->one_loop_ms[i];
  return t;
}

void pokey_player_set_mute(pokey_player *p, int mask)
{
  if (!p)
    return;
  p->mute_mask = mask & 255;
  if (p->asap)
    ASAP_MutePokeyChannels(p->asap, p->mute_mask);
}

int pokey_player_mute(const pokey_player *p)
{
  return p ? p->mute_mask : 0;
}

void pokey_player_set_loop_count(pokey_player *p, int loop_count)
{
  int pos;
  if (!p)
    return;
  if (loop_count < 1) loop_count = 1;
  if (loop_count > 3) loop_count = 3;
  if (loop_count == p->loop_count)
    return;
  pos = p->asap ? ASAP_GetPosition(p->asap) : 0;
  p->loop_count = loop_count;
  recompute_play(p);
  if (p->asap) {
    apply_play(p, p->song);
    if (pos > 0)
      pokey_player_seek_ms(p, pos);
  }
}

int pokey_player_loop_count(const pokey_player *p)
{
  return p ? p->loop_count : POKEY_DEFAULT_LOOPS;
}

int pokey_player_process(pokey_player *p, float *stereo, int count)
{
  int frames, bytes, got, nsamp, i, ch;
  const int16_t *s;

  if (!p || !p->asap || !stereo)
    return 0;
  frames = count / 2;
  if (frames <= 0)
    return 0;
  ch = p->channels > 1 ? 2 : 1;
  bytes = frames * ch * 2;
  if (bytes > (int)sizeof p->scratch)
    bytes = (int)sizeof p->scratch;
  got = ASAP_Generate(p->asap, p->scratch, bytes, ASAPSampleFormat_S16_L_E);
  if (got <= 0)
    return 0;
  nsamp = got / 2;
  s = (const int16_t *)p->scratch;
  if (ch == 2) {
    for (i = 0; i < nsamp; ++i)
      stereo[i] = (float)s[i] * (1.0f / 32768.0f);
    return nsamp;
  }
  for (i = 0; i < nsamp; ++i) {
    float v = (float)s[i] * (1.0f / 32768.0f);
    stereo[i * 2] = v;
    stereo[i * 2 + 1] = v;
  }
  return nsamp * 2;
}

const char *pokey_player_title(const pokey_player *p)
{
  return p ? p->title : "";
}
const char *pokey_player_author(const pokey_player *p)
{
  return p ? p->author : "";
}
const char *pokey_player_date(const pokey_player *p)
{
  return p ? p->date : "";
}
const char *pokey_player_orig_ext(const pokey_player *p)
{
  return p ? p->orig_ext : "";
}
int pokey_player_ntsc(const pokey_player *p)
{
  return p ? p->ntsc : 0;
}
