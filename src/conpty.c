// conpty.c (part of imintty)
// ConPTY (Pseudo Console) detection and infrastructure.
// Provides conpty_available() for runtime feature detection.
// Full ConPTY backend implementation deferred to P2b.

#include "conpty.h"
#include "winpriv.h"

#include <winbase.h>
#include <wincon.h>

/* ConPTY API function pointer types (Win10 1809+, kernel32.dll).
   Dynamically loaded so the exe can run on older Windows too. */
#ifndef HPCON
typedef void * HPCON;
#endif

typedef HRESULT (WINAPI *PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
typedef HRESULT (WINAPI *PFN_ResizePseudoConsole)(HPCON, COORD);
typedef void (WINAPI *PFN_ClosePseudoConsole)(HPCON);

static PFN_CreatePseudoConsole pfn_create_pc;
static PFN_ResizePseudoConsole pfn_resize_pc;
static PFN_ClosePseudoConsole pfn_close_pc;
static bool conpty_initialized;

void
conpty_init(void)
{
  if (conpty_initialized)
    return;
  conpty_initialized = true;

  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  if (!kernel32)
    kernel32 = LoadLibraryW(L"kernel32.dll");
  if (!kernel32)
    return;

  pfn_create_pc = (PFN_CreatePseudoConsole)(void *)
    GetProcAddress(kernel32, "CreatePseudoConsole");
  pfn_resize_pc = (PFN_ResizePseudoConsole)(void *)
    GetProcAddress(kernel32, "ResizePseudoConsole");
  pfn_close_pc = (PFN_ClosePseudoConsole)(void *)
    GetProcAddress(kernel32, "ClosePseudoConsole");
}

bool
conpty_available(void)
{
  conpty_init();
  return (pfn_create_pc != null &&
          pfn_resize_pc != null &&
          pfn_close_pc != null);
}

HPCON
conpty_create(void)
{
  conpty_init();
  if (!pfn_create_pc)
    return null;
  HPCON hpc = null;
  COORD size = { .X = 80, .Y = 25 };
  HRESULT hr = pfn_create_pc(size, null, null, 0, &hpc);
  if (FAILED(hr))
    return null;
  return hpc;
}

void
conpty_resize(HPCON hpc, int cols, int rows)
{
  if (!pfn_resize_pc || !hpc)
    return;
  COORD size = { .X = (short)cols, .Y = (short)rows };
  pfn_resize_pc(hpc, size);
}

void
conpty_close(HPCON hpc)
{
  if (!pfn_close_pc || !hpc)
    return;
  pfn_close_pc(hpc);
}
