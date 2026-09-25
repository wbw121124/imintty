#ifndef CONPTY_H
#define CONPTY_H

#include "win.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize ConPTY function pointers (call once at startup). */
extern void conpty_init(void);

/* Return true if CreatePseudoConsole/ResizePseudoConsole/ClosePseudoConsole
   are available in kernel32.dll (Win10 1809+). */
extern bool conpty_available(void);

/* Lightweight handle wrapper; null means unavailable. */
typedef void * HPCON;

/* Create a pseudo console with default 80x25 size. Returns null on failure. */
extern HPCON conpty_create(void);

/* Resize an existing pseudo console. Safe to call with null hpc. */
extern void conpty_resize(HPCON hpc, int cols, int rows);

/* Close a pseudo console. Safe to call with null hpc. */
extern void conpty_close(HPCON hpc);

#ifdef __cplusplus
}
#endif

#endif
