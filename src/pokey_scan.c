/*
 * Port of asapscan.c -t (silence + hashed POKEY-register windows).
 *
 * asapscan defaults (main(), just above argv parse):
 *   scan_seconds        = 15 * 60
 *   silence_seconds     = 5
 *   loop_check_seconds  = 3 * 60
 *   loop_min_seconds    = 5
 *
 * We scan only POKEY_DETECT_CAP_MS (10 minutes); 10 min is last resort.
 * TIME for a loop is the start of the second matching window (loop point).
 */
#include "pokey_scan.h"
#include "pokey_player.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HASH_BITS 8

/* asapscan defaults we keep */
#define SCAN_SILENCE_SEC     5
#define SCAN_LOOP_CHECK_SEC  (3 * 60)
#define SCAN_LOOP_MIN_SEC    5

static int cycles_per_frame(int ntsc)
{
  return ntsc ? (262 * 114) : (312 * 114);
}

static int main_clock(int ntsc)
{
  return ntsc ? 1789772 : 1773447;
}

static int seconds_to_frames(int seconds, int ntsc)
{
  return (int)((double)seconds * (double)main_clock(ntsc)
               / (double)cycles_per_frame(ntsc));
}

static int frames_to_milliseconds(int frames, int ntsc)
{
  return (int)ceil((double)frames * 1000.0
                   * (double)cycles_per_frame(ntsc)
                   / (double)main_clock(ntsc));
}

static int get_hash(unsigned char *dump, int frame)
{
  unsigned char *p = dump + 18 * frame;
  int i, hash;

  /* asapscan: ignore ultrasound volume-bit on dist 7 + vol 15 */
  for (i = 1; i < 9; i += 2) {
    if ((p[i] & 0xe0) == 0xe0)
      p[i] = (unsigned char)(p[i] & 0xbf);
    if ((p[i + 9] & 0xe0) == 0xe0)
      p[i + 9] = (unsigned char)(p[i + 9] & 0xbf);
  }
  hash = 0;
  for (i = 0; i < 18; i++)
    hash += p[i];
  return hash;
}

static int has_loop_at(const unsigned char *dump, int first_frame,
                       int second_frame, int loop_check_frames)
{
  return memcmp(dump + 18 * first_frame,
                dump + 18 * second_frame,
                (size_t)18 * (size_t)loop_check_frames) == 0;
}

int pokey_scan_from_asap(ASAP *a, int *out_ms, int *out_loop, int *out_found)
{
  const ASAPInfo *info;
  int ntsc;
  int scan_frames, silence_frames, loop_check_frames, loop_min_frames;
  unsigned char *dump;
  int *hash_next;
  int hash_first[1 << HASH_BITS];
  int hash_last[1 << HASH_BITS];
  int frame, i, silence_run, running_hash;
  int cap_ms;

  if (out_ms) *out_ms = POKEY_DETECT_CAP_MS;
  if (out_loop) *out_loop = 0;
  if (out_found) *out_found = 0;
  if (!a)
    return -1;

  info = ASAP_GetInfo(a);
  ntsc = (info && ASAPInfo_IsNtsc(info)) ? 1 : 0;
  cap_ms = POKEY_DETECT_CAP_MS;

  /* 10-minute cap (not asapscan's 15 min). Enough for 2:49 + 3:00 window. */
  scan_frames = seconds_to_frames(cap_ms / 1000, ntsc);
  silence_frames = seconds_to_frames(SCAN_SILENCE_SEC, ntsc);
  loop_check_frames = seconds_to_frames(SCAN_LOOP_CHECK_SEC, ntsc);
  loop_min_frames = seconds_to_frames(SCAN_LOOP_MIN_SEC, ntsc);
  if (scan_frames < loop_check_frames + loop_min_frames)
    scan_frames = loop_check_frames + loop_min_frames;

  dump = (unsigned char *)malloc((size_t)scan_frames * 18u);
  hash_next = (int *)malloc((size_t)scan_frames * sizeof(int));
  if (!dump || !hash_next) {
    free(dump);
    free(hash_next);
    return -1;
  }

  for (i = 0; i < (1 << HASH_BITS); i++)
    hash_first[i] = -1;

  silence_run = 0;
  running_hash = 0;

  for (frame = 0; frame < scan_frames; frame++) {
    int silent;

    ASAP_XmpDo6502Frame(a);
    silent = ASAP_XmpStorePokeyRegs(a, dump + 18 * frame);

    if (silent) {
      silence_run++;
      /* do not trigger at the initial silence */
      if (silence_run >= silence_frames && silence_run < frame) {
        int start = frame + 1 - silence_run;
        if (start < 0) start = 0;
        if (out_ms) *out_ms = frames_to_milliseconds(start, ntsc);
        if (out_loop) *out_loop = 0;
        if (out_found) *out_found = 1;
        free(dump);
        free(hash_next);
        return 0;
      }
    } else {
      silence_run = 0;
    }

    if (frame >= loop_check_frames) {
      int second_frame = frame - loop_check_frames;
      int first_frame;

      running_hash &= (1 << HASH_BITS) - 1;
      for (first_frame = hash_first[running_hash];
           first_frame >= 0;
           first_frame = hash_next[first_frame]) {
        if (has_loop_at(dump, first_frame, second_frame, loop_check_frames)) {
          int loop_len = second_frame - first_frame;
          if (loop_len >= loop_min_frames) {
            if (out_ms)
              *out_ms = frames_to_milliseconds(second_frame, ntsc);
            if (out_loop) *out_loop = 1;
            if (out_found) *out_found = 1;
            free(dump);
            free(hash_next);
            return 0;
          }
          if (loop_len == 1) {
            /* frozen POKEY regs — ultrasound / hung player */
            if (out_ms)
              *out_ms = frames_to_milliseconds(first_frame, ntsc);
            if (out_loop) *out_loop = 0;
            if (out_found) *out_found = 1;
            free(dump);
            free(hash_next);
            return 0;
          }
        }
      }
      if (hash_first[running_hash] >= 0)
        hash_next[hash_last[running_hash]] = second_frame;
      else
        hash_first[running_hash] = second_frame;
      hash_next[second_frame] = -1;
      hash_last[running_hash] = second_frame;
      running_hash -= get_hash(dump, second_frame);
    }
    running_hash += get_hash(dump, frame);
  }

  /* neither silence nor loop */
  if (out_ms) *out_ms = cap_ms;
  if (out_loop) *out_loop = 0;
  if (out_found) *out_found = 0;
  free(dump);
  free(hash_next);
  return 0;
}
