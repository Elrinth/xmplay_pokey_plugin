/*
 * xmp-pokey — crash-safe native XMPlay input plugin for Atari 8-bit
 * POKEY music via official ASAP (Another Slight Atari Player).
 *
 * This is NOT the official ASAP XMPlay plugin (support.xmplay.com file 638,
 * xmp-asap 6.0.3). It uses the ASAP engine but exists to:
 *   - detect length when TIME is missing (songs no longer play forever)
 *   - loop 1 / 2 / 3 times
 *   - mute POKEY channels
 *   - NSF-style multi-track (Shift+arrows)
 *
 * Classic XMPlay is 32-bit only. DllMain only DisableThreadLibraryCalls.
 */
#if defined(__GNUC__)
#define XMPIN_GetInterface XMPIN_GetInterface_Declared
#endif
#include "xmpin.h"
#if defined(__GNUC__)
#undef XMPIN_GetInterface
#endif

#include "pokey_player.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define PLUGIN_NAME    "POKEY (Atari 8-bit)"
#define PLUGIN_VERSION "1.0.2"
#define MAX_MODULE_BYTES ((size_t)65000)
#define INFO_WRITE_MAX 32766

typedef struct {
  int loop_count; /* 1, 2, 3 */
  int mute_base;  /* bits 0-3 */
  int mute_extra; /* bits 4-7 */
} pokey_cfg_t;

static XMPFUNC_IN   *xmpfin;
static XMPFUNC_MISC *xmpfmisc;
static XMPFUNC_FILE *xmpffile;

static pokey_player *g_play;
static char          g_name_hint[512];
static pokey_cfg_t   g_cfg = { POKEY_DEFAULT_LOOPS, 0, 0 };

#ifdef _WIN32
static HINSTANCE g_hinst;
#endif

static int cfg_mask(void)
{
  return (g_cfg.mute_base & 15) | ((g_cfg.mute_extra & 15) << 4);
}

static void clamp_cfg(void)
{
  if (g_cfg.loop_count < 1) g_cfg.loop_count = 1;
  if (g_cfg.loop_count > 3) g_cfg.loop_count = 3;
  g_cfg.mute_base &= 15;
  g_cfg.mute_extra &= 15;
}

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

static void sanitize_line(char *s)
{
  if (!s) return;
  for (; *s; ++s)
    if (*s == '\t' || *s == '\r' || *s == '\n')
      *s = ' ';
}

static void write_kv(char **cursor, char *end, const char *name, const char *value)
{
  size_t nl, vl, need;
  if (!cursor || !*cursor || !end || !name || !value || !value[0])
    return;
  nl = strlen(name);
  vl = strlen(value);
  need = nl + 1 + vl + 1;
  if (*cursor + need >= end)
    return;
  memcpy(*cursor, name, nl); *cursor += nl;
  **cursor = '\t'; *cursor += 1;
  memcpy(*cursor, value, vl); *cursor += vl;
  **cursor = '\r'; *cursor += 1;
  **cursor = '\0';
}

static void *xmp_alloc(DWORD n)
{
  if (!xmpfmisc || !xmpfmisc->Alloc || n == 0)
    return NULL;
  return xmpfmisc->Alloc(n);
}

static void remember_hint(const char *filename)
{
  size_t n;
  g_name_hint[0] = '\0';
  if (!filename || !filename[0])
    return;
  n = strlen(filename);
  if (n >= sizeof g_name_hint)
    n = sizeof g_name_hint - 1;
  memcpy(g_name_hint, filename, n);
  g_name_hint[n] = '\0';
}

static int slurp_xmpfile(XMPFILE file, unsigned char **out, size_t *out_len)
{
  DWORD type, sz, got, pos;
  unsigned char *buf;
  if (out) *out = NULL;
  if (out_len) *out_len = 0;
  if (!file || !out || !out_len || !xmpffile || !xmpffile->Read)
    return 0;
  type = xmpffile->GetType(file);
  if (type == XMPFILE_TYPE_MEMORY) {
    const void *mem;
    if (!xmpffile->GetMemory || !xmpffile->GetSize)
      return 0;
    mem = xmpffile->GetMemory(file);
    sz = xmpffile->GetSize(file);
    if (!mem || sz < 4 || (size_t)sz > MAX_MODULE_BYTES)
      return 0;
    buf = (unsigned char *)malloc(sz);
    if (!buf) return 0;
    memcpy(buf, mem, sz);
    *out = buf;
    *out_len = sz;
    return 1;
  }
  sz = xmpffile->GetSize ? xmpffile->GetSize(file) : 0;
  pos = xmpffile->Tell ? xmpffile->Tell(file) : 0;
  if (xmpffile->Seek)
    xmpffile->Seek(file, 0);
  if (sz > 0) {
    if (sz < 4 || (size_t)sz > MAX_MODULE_BYTES) {
      if (xmpffile->Seek) xmpffile->Seek(file, pos);
      return 0;
    }
    buf = (unsigned char *)malloc(sz);
    if (!buf) {
      if (xmpffile->Seek) xmpffile->Seek(file, pos);
      return 0;
    }
    got = xmpffile->Read(file, buf, sz);
    if (xmpffile->Seek) xmpffile->Seek(file, pos);
    if (got < 4) { free(buf); return 0; }
    *out = buf;
    *out_len = got;
    return 1;
  }
  {
    size_t cap = 4096, total = 0;
    buf = (unsigned char *)malloc(cap);
    if (!buf) return 0;
    for (;;) {
      DWORD chunk;
      if (total == cap) {
        size_t ncap = cap * 2;
        unsigned char *nb;
        if (ncap > MAX_MODULE_BYTES) ncap = MAX_MODULE_BYTES;
        if (ncap <= cap) { free(buf); return 0; }
        nb = (unsigned char *)realloc(buf, ncap);
        if (!nb) { free(buf); return 0; }
        buf = nb;
        cap = ncap;
      }
      chunk = xmpffile->Read(file, buf + total, (DWORD)(cap - total));
      if (chunk == 0) break;
      total += chunk;
      if (total >= MAX_MODULE_BYTES) break;
    }
    if (xmpffile->Seek) xmpffile->Seek(file, pos);
    if (total < 4) { free(buf); return 0; }
    *out = buf;
    *out_len = total;
    return 1;
  }
}

static XMPFILE open_if_needed(const char *filename, XMPFILE file, int *opened)
{
  *opened = 0;
  if (file) return file;
  if (!filename || !xmpffile || !xmpffile->Open)
    return NULL;
  file = xmpffile->Open(filename);
  if (file) *opened = 1;
  return file;
}

static void close_if_opened(XMPFILE file, int opened)
{
  if (opened && file && xmpffile && xmpffile->Close)
    xmpffile->Close(file);
}

static void apply_cfg(void)
{
  if (!g_play) return;
  pokey_player_set_mute(g_play, cfg_mask());
  pokey_player_set_loop_count(g_play, g_cfg.loop_count);
}

static void unload_playback(void)
{
  if (g_play) {
    pokey_player_close(g_play);
    g_play = NULL;
  }
  g_name_hint[0] = '\0';
}

static void append_tag(char **p, char *end, const char *key, const char *val)
{
  size_t kl, vl;
  if (!p || !*p || !end || !key || !val || !val[0])
    return;
  kl = strlen(key);
  vl = strlen(val);
  if (*p + kl + 1 + vl + 1 + 1 >= end)
    return;
  memcpy(*p, key, kl); *p += kl;
  **p = '\0'; *p += 1;
  memcpy(*p, val, vl); *p += vl;
  **p = '\0'; *p += 1;
}

static char *finish_tags(char *stack, char *p, size_t stack_sz)
{
  char *end = stack + stack_sz;
  char *out;
  size_t n;
  if (p + 1 < end)
    *p++ = '\0';
  n = (size_t)(p - stack);
  out = (char *)xmp_alloc((DWORD)n);
  if (!out) return NULL;
  memcpy(out, stack, n);
  return out;
}

static char *build_tags_info(const pokey_info *inf, int track0)
{
  char stack[8192];
  char *p = stack;
  char *end = stack + sizeof stack;
  char trk[16];
  (void)track0;
  if (!xmpfmisc) return NULL;
  append_tag(&p, end, "filetype", "SAP");
  append_tag(&p, end, "title", inf->title);
  append_tag(&p, end, "artist", inf->author);
  append_tag(&p, end, "date", inf->date);
  if (inf->songs > 1) {
    snprintf(trk, sizeof trk, "%d", track0 + 1);
    append_tag(&p, end, "track", trk);
  }
  return finish_tags(stack, p, sizeof stack);
}

static char *build_tags_play(pokey_player *pl)
{
  char stack[8192];
  char *p = stack;
  char *end = stack + sizeof stack;
  char trk[16];
  if (!pl || !xmpfmisc) return NULL;
  append_tag(&p, end, "filetype", "SAP");
  append_tag(&p, end, "title", pokey_player_title(pl));
  append_tag(&p, end, "artist", pokey_player_author(pl));
  append_tag(&p, end, "date", pokey_player_date(pl));
  if (pokey_player_songs(pl) > 1) {
    snprintf(trk, sizeof trk, "%d", pokey_player_song(pl) + 1);
    append_tag(&p, end, "track", trk);
  }
  return finish_tags(stack, p, sizeof stack);
}

static void set_length_now(int play_ms)
{
  float sec;
  if (!xmpfin || !xmpfin->SetLength || play_ms <= 0)
    return;
  sec = (float)play_ms / 1000.0f;
  if (sec > 0.0f && sec < 86400.0f)
    xmpfin->SetLength(sec, TRUE);
}

/* ---- XMPIN methods ---------------------------------------------------- */

static void WINAPI pokey_About(HWND win)
{
  char buf[1600];
  snprintf(buf, sizeof buf,
    PLUGIN_NAME " " PLUGIN_VERSION "\r\n"
    "Native XMPlay input plugin for Atari 8-bit POKEY music.\r\n"
    "Engine: official ASAP " /* version via credits */ "by Piotr Fusik.\r\n\r\n"
    "This is NOT the official ASAP XMPlay plugin (support.xmplay.com\r\n"
    "file 638, xmp-asap 6.0.3). That plugin leaves songs without a TIME\r\n"
    "tag (for example older rips of Spy vs Spy) playing forever.\r\n\r\n"
    "This plugin uses the ASAP engine but is a separate input:\r\n"
    "  - measures length via 2s silence (10-minute cap) when TIME is\r\n"
    "    missing or is the 3:00 stub (ASMA / xmp-asap default)\r\n"
    "  - real TIME tags (e.g. Spy vs Spy 00:19.25) are kept as-is\r\n"
    "  - loops 1 / 2 / 3 times (default 1; non-looping TIME plays once)\r\n"
    "  - playlist/info length is the calculated one-loop duration\r\n"
    "  - mute POKEY 1-4 and extra/stereo POKEY 1-4\r\n"
    "  - NSF-style tracks (Shift+Left / Shift+Right)\r\n"
    "  - never calls ASAP_PlaySong(..., -1)\r\n\r\n"
    "Formats: sap cmc cm3 cmr cms dmc dlt fc mpt mpd rmt tmc tm8 tm2\r\n"
    "Atari .fc only — Amiga Future Composer is rejected.\r\n"
    "32-bit XMPlay only (PE32 i386).\r\n"
    "License: GPLv2+ (ASAP is GPLv2+).");
#ifdef _WIN32
  MessageBoxA(win, buf, PLUGIN_NAME, MB_OK | MB_ICONINFORMATION);
#else
  (void)win;
  (void)buf;
#endif
}

#ifdef _WIN32
#define IDC_LOOP1  1001
#define IDC_LOOP2  1002
#define IDC_LOOP3  1003
#define IDC_MUTE0  1010

static INT_PTR CALLBACK cfg_dlg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  int i;
  (void)lp;
  switch (msg) {
  case WM_INITDIALOG:
    CheckRadioButton(hwnd, IDC_LOOP1, IDC_LOOP3,
                     IDC_LOOP1 + (g_cfg.loop_count - 1));
    for (i = 0; i < 8; ++i) {
      int bit = (i < 4) ? (g_cfg.mute_base >> i) : (g_cfg.mute_extra >> (i - 4));
      CheckDlgButton(hwnd, IDC_MUTE0 + i, (bit & 1) ? BST_CHECKED : BST_UNCHECKED);
    }
    return TRUE;
  case WM_COMMAND:
    if (LOWORD(wp) == IDOK) {
      if (IsDlgButtonChecked(hwnd, IDC_LOOP1) == BST_CHECKED)
        g_cfg.loop_count = 1;
      else if (IsDlgButtonChecked(hwnd, IDC_LOOP3) == BST_CHECKED)
        g_cfg.loop_count = 3;
      else
        g_cfg.loop_count = 2;
      g_cfg.mute_base = 0;
      g_cfg.mute_extra = 0;
      for (i = 0; i < 4; ++i)
        if (IsDlgButtonChecked(hwnd, IDC_MUTE0 + i) == BST_CHECKED)
          g_cfg.mute_base |= (1 << i);
      for (i = 0; i < 4; ++i)
        if (IsDlgButtonChecked(hwnd, IDC_MUTE0 + 4 + i) == BST_CHECKED)
          g_cfg.mute_extra |= (1 << i);
      clamp_cfg();
      apply_cfg();
      if (g_play)
        set_length_now(pokey_player_play_ms(g_play, pokey_player_song(g_play)));
      EndDialog(hwnd, IDOK);
      return TRUE;
    }
    if (LOWORD(wp) == IDCANCEL) {
      EndDialog(hwnd, IDCANCEL);
      return TRUE;
    }
    break;
  }
  return FALSE;
}

static void WINAPI pokey_Config(HWND win)
{
  WORD *p;
  DLGTEMPLATE *dlg;
  unsigned char raw[1024];
  memset(raw, 0, sizeof raw);
  dlg = (DLGTEMPLATE *)raw;
  dlg->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
  dlg->cdit = 18;
  dlg->x = 10; dlg->y = 10; dlg->cx = 220; dlg->cy = 118;
  p = (WORD *)(dlg + 1);
  *p++ = 0;
  *p++ = 0;
  {
    const wchar_t *cap = L"POKEY";
    size_t i;
    for (i = 0; cap[i]; ++i) *p++ = (WORD)cap[i];
    *p++ = 0;
  }
  *p++ = 9;
  {
    const wchar_t *fnt = L"MS Shell Dlg";
    size_t i;
    for (i = 0; fnt[i]; ++i) *p++ = (WORD)fnt[i];
    *p++ = 0;
  }
#define ADDCTL(_id, _x, _y, _w, _h, _style, _clsid, _title) do { \
    DLGITEMTEMPLATE *item; \
    if (((uintptr_t)p) & 3) p = (WORD *)((((uintptr_t)p) + 3) & ~(uintptr_t)3); \
    item = (DLGITEMTEMPLATE *)p; \
    item->style = WS_CHILD | WS_VISIBLE | (_style); \
    item->x = (short)(_x); item->y = (short)(_y); \
    item->cx = (short)(_w); item->cy = (short)(_h); \
    item->id = (WORD)(_id); \
    p = (WORD *)(item + 1); \
    *p++ = 0xFFFF; *p++ = (WORD)(_clsid); \
    { const wchar_t *_t = (_title); size_t _i; \
      for (_i = 0; _t[_i]; ++_i) *p++ = (WORD)_t[_i]; *p++ = 0; } \
    *p++ = 0; \
  } while (0)
  ADDCTL(-1, 8, 8, 40, 10, 0, 0x0082, L"Loops");
  ADDCTL(IDC_LOOP1, 50, 6, 28, 12, WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP, 0x0080, L"1");
  ADDCTL(IDC_LOOP2, 82, 6, 28, 12, WS_TABSTOP | BS_AUTORADIOBUTTON, 0x0080, L"2");
  ADDCTL(IDC_LOOP3, 114, 6, 28, 12, WS_TABSTOP | BS_AUTORADIOBUTTON, 0x0080, L"3");
  ADDCTL(-1, 8, 24, 200, 10, 0, 0x0082, L"Mute POKEY 1-4");
  ADDCTL(IDC_MUTE0 + 0, 8, 36, 40, 12, WS_TABSTOP | BS_AUTOCHECKBOX, 0x0080, L"1");
  ADDCTL(IDC_MUTE0 + 1, 50, 36, 40, 12, WS_TABSTOP | BS_AUTOCHECKBOX, 0x0080, L"2");
  ADDCTL(IDC_MUTE0 + 2, 92, 36, 40, 12, WS_TABSTOP | BS_AUTOCHECKBOX, 0x0080, L"3");
  ADDCTL(IDC_MUTE0 + 3, 134, 36, 40, 12, WS_TABSTOP | BS_AUTOCHECKBOX, 0x0080, L"4");
  ADDCTL(-1, 8, 54, 200, 10, 0, 0x0082, L"Mute extra POKEY 1-4");
  ADDCTL(IDC_MUTE0 + 4, 8, 66, 40, 12, WS_TABSTOP | BS_AUTOCHECKBOX, 0x0080, L"1");
  ADDCTL(IDC_MUTE0 + 5, 50, 66, 40, 12, WS_TABSTOP | BS_AUTOCHECKBOX, 0x0080, L"2");
  ADDCTL(IDC_MUTE0 + 6, 92, 66, 40, 12, WS_TABSTOP | BS_AUTOCHECKBOX, 0x0080, L"3");
  ADDCTL(IDC_MUTE0 + 7, 134, 66, 40, 12, WS_TABSTOP | BS_AUTOCHECKBOX, 0x0080, L"4");
  ADDCTL(IDOK, 110, 96, 46, 14, WS_TABSTOP | BS_DEFPUSHBUTTON, 0x0080, L"OK");
  ADDCTL(IDCANCEL, 162, 96, 46, 14, WS_TABSTOP | BS_PUSHBUTTON, 0x0080, L"Cancel");
#undef ADDCTL
  DialogBoxIndirectParamA(g_hinst, dlg, win, cfg_dlg, 0);
}
#else
static void WINAPI pokey_Config(HWND win) { (void)win; }
#endif

static BOOL WINAPI pokey_CheckFile(const char *filename, XMPFILE file)
{
  unsigned char *data = NULL;
  size_t len = 0;
  int opened = 0;
  int ok;

  file = open_if_needed(filename, file, &opened);
  if (!file)
    return FALSE;
  if (!slurp_xmpfile(file, &data, &len)) {
    close_if_opened(file, opened);
    return FALSE;
  }
  close_if_opened(file, opened);
  /* Must Load, not extension-only — Atari .fc must not steal Amiga .fc. */
  ok = pokey_probe(filename, data, len);
  free(data);
  return ok ? TRUE : FALSE;
}

/* FACE 4 GetFileInfo: same contract as xmp-sc68 / Ian Luck (un4seen):
 * Alloc an array of floats (seconds) via XMPFUNC_MISC.Alloc, one per
 * subsong; return song count | XMPIN_INFO_NOSUBTAGS. XMPlay default
 * playlist length is 3:00 when this array is missing or 0. */
static DWORD WINAPI pokey_GetFileInfo(const char *filename, XMPFILE file,
                                      float **length, char **tags)
{
  unsigned char *data = NULL;
  size_t len = 0;
  int opened = 0;
  pokey_info inf;
  int n, i;

  if (length) *length = NULL;
  if (tags) *tags = NULL;

  file = open_if_needed(filename, file, &opened);
  if (!file)
    return 0;
  if (!slurp_xmpfile(file, &data, &len)) {
    close_if_opened(file, opened);
    return 0;
  }
  close_if_opened(file, opened);

  if (pokey_analyze(filename, data, len, g_cfg.loop_count, &inf) != 0) {
    free(data);
    return 0;
  }
  free(data);

  n = inf.songs > 0 ? inf.songs : 1;
  if (length) {
    float *lens = (float *)xmp_alloc((DWORD)(sizeof(float) * (unsigned)n));
    if (lens) {
      for (i = 0; i < n; ++i)
        lens[i] = (float)inf.one_loop_ms[i] / 1000.0f;
    }
    *length = lens;
  }
  if (tags)
    *tags = build_tags_info(&inf, 0);
  return (DWORD)n | XMPIN_INFO_NOSUBTAGS;
}

static DWORD WINAPI pokey_Open(const char *filename, XMPFILE file)
{
  unsigned char *data = NULL;
  size_t len = 0;
  int opened = 0;

  unload_playback();
  remember_hint(filename);

  file = open_if_needed(filename, file, &opened);
  if (!file)
    return 0;
  if (!slurp_xmpfile(file, &data, &len)) {
    close_if_opened(file, opened);
    return 0;
  }
  close_if_opened(file, opened);

  g_play = pokey_player_open(filename, data, len, g_cfg.loop_count, cfg_mask());
  free(data);
  if (!g_play)
    return 0;
  set_length_now(pokey_player_play_ms(g_play, pokey_player_song(g_play)));
  return 2; /* stereo */
}

static void WINAPI pokey_Close(void)
{
  unload_playback();
}

static void WINAPI pokey_SetFormat(XMPFORMAT *form)
{
  if (!form)
    return;
  if (!g_play) {
    form->rate = 0;
    form->chan = 0;
    form->res = 0;
    form->chanmask = 0;
    return;
  }
  form->rate = (DWORD)pokey_player_rate(g_play);
  form->chan = 2;
  form->res = 4; /* float */
  form->chanmask = 0;
}

static char *WINAPI pokey_GetTags(void)
{
  if (!g_play)
    return NULL;
  return build_tags_play(g_play);
}

static void WINAPI pokey_GetInfoText(char *format, char *length)
{
  char tmp[256];
  int m, s, play;
  if (format) format[0] = '\0';
  if (length) length[0] = '\0';
  if (!g_play)
    return;
  if (format) {
    const char *ext = pokey_player_orig_ext(g_play);
    snprintf(tmp, sizeof tmp, "SAP / POKEY%s%s  %s",
             ext && ext[0] ? "  " : "",
             ext && ext[0] ? ext : "",
             pokey_player_ntsc(g_play) ? "NTSC" : "PAL");
    sanitize_line(tmp);
    bounded_copy(format, 256, tmp);
  }
  if (length) {
    play = pokey_player_one_loop_ms(g_play, pokey_player_song(g_play));
    m = play / 60000;
    s = (play / 1000) % 60;
    if (pokey_player_songs(g_play) > 1)
      snprintf(tmp, sizeof tmp, "%d:%02d  track %d/%d",
               m, s, pokey_player_song(g_play) + 1, pokey_player_songs(g_play));
    else
      snprintf(tmp, sizeof tmp, "%d:%02d", m, s);
    sanitize_line(tmp);
    bounded_copy(length, 256, tmp);
  }
}

static void WINAPI pokey_GetGeneralInfo(char *buf)
{
  char local[4096];
  char *p, *end;
  char num[32];
  if (!buf) return;
  buf[0] = '\0';
  if (!g_play) return;
  p = local;
  end = local + sizeof local - 2;
  local[0] = '\0';
  write_kv(&p, end, "Title", pokey_player_title(g_play));
  write_kv(&p, end, "Author", pokey_player_author(g_play));
  write_kv(&p, end, "Date", pokey_player_date(g_play));
  write_kv(&p, end, "Format", "SAP / POKEY");
  if (pokey_player_orig_ext(g_play)[0])
    write_kv(&p, end, "Original format", pokey_player_orig_ext(g_play));
  write_kv(&p, end, "TV", pokey_player_ntsc(g_play) ? "NTSC" : "PAL");
  snprintf(num, sizeof num, "%d", pokey_player_channels(g_play));
  write_kv(&p, end, "Channels", num);
  if (pokey_player_songs(g_play) > 1) {
    snprintf(num, sizeof num, "%d", pokey_player_songs(g_play));
    write_kv(&p, end, "Tracks", num);
    snprintf(num, sizeof num, "%d", pokey_player_song(g_play) + 1);
    write_kv(&p, end, "Current track", num);
  }
  snprintf(num, sizeof num, "%d", pokey_player_loop_count(g_play));
  write_kv(&p, end, "Loop count", num);
  write_kv(&p, end, "Player", PLUGIN_NAME " " PLUGIN_VERSION);
  write_kv(&p, end, "Engine", "official ASAP (Piotr Fusik)");
  write_kv(&p, end, "Note", "Not the official ASAP XMPlay plugin (file 638)");
  bounded_copy(buf, INFO_WRITE_MAX, local);
}

static void WINAPI pokey_GetMessage(char *buf)
{
  if (!buf) return;
  buf[0] = '\0';
}

static double WINAPI pokey_GetGranularity(void)
{
  return 0.001;
}

static double WINAPI pokey_SetPosition(DWORD pos)
{
  int sub;
  int ms;

  if (!g_play)
    return -1.0;

  if (pos == (DWORD)XMPIN_POS_LOOP || pos == (DWORD)XMPIN_POS_AUTOLOOP)
    return -2.0;

  if (pos & XMPIN_POS_SUBSONG) {
    sub = (int)(pos & 0xFFFFu);
    if (pokey_player_set_song(g_play, sub) != 0)
      return -1.0;
    set_length_now(pokey_player_play_ms(g_play, sub));
    if (xmpfin && xmpfin->UpdateTitle)
      xmpfin->UpdateTitle(NULL);
    return 0.0;
  }

  ms = (int)pos; /* granularity 0.001 → pos is milliseconds */
  ms = pokey_player_seek_ms(g_play, ms);
  if (ms < 0)
    return -1.0;
  return (double)ms / 1000.0;
}

static DWORD WINAPI pokey_Process(float *buf, DWORD count)
{
  int got;
  if (!buf || !g_play)
    return 0;
  got = pokey_player_process(g_play, buf, (int)count);
  if (got <= 0)
    return 0;
  return (DWORD)got;
}

static DWORD WINAPI pokey_GetSubSongs(float *length)
{
  if (!g_play)
    return 0;
  if (length)
    *length = (float)pokey_player_total_one_loop_ms(g_play) / 1000.0f;
  return (DWORD)pokey_player_songs(g_play);
}

static DWORD WINAPI pokey_GetConfig(void *config)
{
  if (config)
    memcpy(config, &g_cfg, sizeof g_cfg);
  return (DWORD)sizeof g_cfg;
}

static void WINAPI pokey_SetConfig(void *config, DWORD size)
{
  if (!config || size < sizeof g_cfg)
    return;
  memcpy(&g_cfg, config, sizeof g_cfg);
  clamp_cfg();
  apply_cfg();
}

static const char g_exts[] =
  "Atari SAP / POKEY\0sap/cmc/cm3/cmr/cms/dmc/dlt/fc/mpt/mpd/rmt/tmc/tm8/tm2";

static XMPIN g_xmpin = {
  XMPIN_FLAG_CONFIG,
  PLUGIN_NAME " " PLUGIN_VERSION,
  g_exts,
  pokey_About,
  pokey_Config,
  pokey_CheckFile,
  pokey_GetFileInfo,
  pokey_Open,
  pokey_Close,
  NULL,
  pokey_SetFormat,
  pokey_GetTags,
  pokey_GetInfoText,
  pokey_GetGeneralInfo,
  pokey_GetMessage,
  pokey_SetPosition,
  pokey_GetGranularity,
  NULL,
  pokey_Process,
  NULL,
  NULL,
  pokey_GetSubSongs,
  NULL,
  NULL,
  NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  pokey_GetConfig,
  pokey_SetConfig,
  NULL
};

static XMPIN *WINAPI xmpin_get_interface_impl(DWORD face, InterfaceProc faceproc)
{
  if (face != XMPIN_FACE)
    return NULL;
  if (!faceproc)
    return NULL;
  xmpfin   = (XMPFUNC_IN *)faceproc(XMPFUNC_IN_FACE);
  xmpfmisc = (XMPFUNC_MISC *)faceproc(XMPFUNC_MISC_FACE);
  xmpffile = (XMPFUNC_FILE *)faceproc(XMPFUNC_FILE_FACE);
  if (!xmpfin || !xmpfmisc || !xmpffile)
    return NULL;
  if (!xmpfmisc->Alloc || !xmpffile->Read)
    return NULL;
  return &g_xmpin;
}

extern "C" {

BOOL WINAPI DllMain(HINSTANCE hDLL, DWORD reason, LPVOID reserved)
{
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
#ifdef _WIN32
    g_hinst = hDLL;
    DisableThreadLibraryCalls(hDLL);
#endif
  }
  return TRUE;
}

#if defined(__GNUC__) && defined(_WIN32) && !defined(_WIN64)
XMPIN *WINAPI XMPIN_GetInterface_(DWORD face, InterfaceProc faceproc)
{
  return xmpin_get_interface_impl(face, faceproc);
}
#if __GNUC__ >= 8
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattribute-alias"
#endif
__attribute__((dllexport)) void XMPIN_GetInterface(void)
  __attribute__((alias("XMPIN_GetInterface_@8")));
#if __GNUC__ >= 8
#pragma GCC diagnostic pop
#endif
#else
__declspec(dllexport) XMPIN *WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
  return xmpin_get_interface_impl(face, faceproc);
}
#endif

} /* extern "C" */
