/*
 * Host-testable ASAP wrapper used by xmp-pokey (native XMPlay plugin).
 * Not the official ASAP XMPlay plugin.
 */
#ifndef POKEY_PLAYER_H
#define POKEY_PLAYER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POKEY_MAX_SONGS        32
#define POKEY_MAX_MODULE       65000
#define POKEY_DETECT_CAP_MS    (10 * 60 * 1000)
#define POKEY_DEFAULT_LOOPS    1
#define POKEY_STR              128
/* Official xmp-asap DEFAULT_SONG_LENGTH is 180s. ASMA often stamps TIME 3:00
 * as a stub, not a measured loop. Treat that window as dummy. */
#define POKEY_DUMMY_TIME_MS    180000
#define POKEY_DUMMY_TIME_LO    179000
#define POKEY_DUMMY_TIME_HI    181000

typedef struct pokey_info {
  int   songs;
  int   default_song;
  int   channels;                 /* 1 or 2 */
  int   tagged_ms[POKEY_MAX_SONGS]; /* ASAPInfo_GetDuration; -1 if none */
  int   one_loop_ms[POKEY_MAX_SONGS];
  int   play_ms[POKEY_MAX_SONGS];
  int   loops[POKEY_MAX_SONGS];   /* ASAPInfo_GetLoop */
  int   detected[POKEY_MAX_SONGS];/* 1 if loop/silence-detect was used */
  char  title[POKEY_STR];
  char  author[POKEY_STR];
  char  date[POKEY_STR];
  char  orig_ext[16];
  int   ntsc;
  int   rate_hz;
} pokey_info;

/* Load-only probe. Returns 1 if ASAP accepts the bytes. */
int pokey_probe(const char *filename, const unsigned char *data, size_t len);

/* Fill metadata + per-song lengths (loop/silence-detect unknown / dummy 3:00). */
int pokey_analyze(const char *filename, const unsigned char *data, size_t len,
                  int loop_count, pokey_info *out);

int pokey_play_ms_from(int one_loop_ms, int loops_flag, int loop_count, int unknown);

/* 1 if ms is the 3:00 stub window (179000-181000). */
int pokey_is_dummy_time(int ms);

typedef struct pokey_player pokey_player;

pokey_player *pokey_player_open(const char *filename, const unsigned char *data,
                                size_t len, int loop_count, int mute_mask);
void          pokey_player_close(pokey_player *p);

int    pokey_player_songs(const pokey_player *p);
int    pokey_player_song(const pokey_player *p); /* 0-based */
int    pokey_player_channels(const pokey_player *p);
int    pokey_player_rate(const pokey_player *p);
int    pokey_player_set_song(pokey_player *p, int song0);
int    pokey_player_one_loop_ms(const pokey_player *p, int song0);
int    pokey_player_play_ms(const pokey_player *p, int song0);
int    pokey_player_detected(const pokey_player *p, int song0);
int    pokey_player_was_loop(const pokey_player *p, int song0);
int    pokey_player_position_ms(const pokey_player *p);
int    pokey_player_seek_ms(pokey_player *p, int ms);
int    pokey_player_total_play_ms(const pokey_player *p);
int    pokey_player_total_one_loop_ms(const pokey_player *p);

void   pokey_player_set_mute(pokey_player *p, int mask);
int    pokey_player_mute(const pokey_player *p);
void   pokey_player_set_loop_count(pokey_player *p, int loop_count);
int    pokey_player_loop_count(const pokey_player *p);

/* Decode stereo float samples. count = number of floats (L+R).
 * Returns floats written (0 = end). */
int    pokey_player_process(pokey_player *p, float *stereo, int count);

const char *pokey_player_title(const pokey_player *p);
const char *pokey_player_author(const pokey_player *p);
const char *pokey_player_date(const pokey_player *p);
const char *pokey_player_orig_ext(const pokey_player *p);
int         pokey_player_ntsc(const pokey_player *p);

#ifdef __cplusplus
}
#endif
#endif
