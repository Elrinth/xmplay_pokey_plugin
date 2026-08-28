/*
 * asapscan -t style silence + POKEY-register loop detect.
 * Does not generate PCM.
 */
#ifndef POKEY_SCAN_H
#define POKEY_SCAN_H

#include "asap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Tiny exports from patched asap.c */
int ASAP_XmpDo6502Frame(ASAP *self);
int ASAP_XmpStorePokeyRegs(const ASAP *self, unsigned char *out18);

/*
 * Scan an already PlaySong'd ASAP instance.
 * out_ms: silence start, loop point, or POKEY_DETECT_CAP_MS
 * out_loop: 1 if a register loop was found
 * out_found: 1 if silence or loop (0 = hit cap)
 * Returns 0 on success, -1 on OOM / bad args.
 */
int pokey_scan_from_asap(ASAP *a, int *out_ms, int *out_loop, int *out_found);

#ifdef __cplusplus
}
#endif
#endif
