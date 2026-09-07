/* ------------------------------------------------------------------------
 * gpu.c - GPU utilisation and video memory acquisition.
 * Pure acquisition. No Win32 UI calls: see gpu.h.
 * ------------------------------------------------------------------------ */
#include "gpu.h"
#include <shlwapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include <wchar.h>
