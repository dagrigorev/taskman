/* ------------------------------------------------------------------------
 * gdiplusapi.h - hand written declarations for the GDI+ flat (C) API.
 *
 * <gdiplus.h> is a C++ header and is not usable from this C11 codebase, so
 * the handful of entry points needed for antialiased chart drawing are
 * declared here instead.  Every type carries a CTM_ prefix so it can never
 * collide with the SDK header if something includes it later.
 *
 * Everything is reached through GetProcAddress on gdiplus.dll, so the
 * executable keeps its "no load-time dependency" property: if GDI+ is
 * missing or an export changes, the charts fall back to plain GDI instead
 * of failing to load.
 * ------------------------------------------------------------------------ */
#ifndef CTM_GDIPLUSAPI_H
#define CTM_GDIPLUSAPI_H

#include <windows.h>
#include <stddef.h>

typedef INT   CTM_GpStatus;             /* 0 == Ok                           */
typedef FLOAT CTM_REAL;
typedef DWORD CTM_ARGB;                 /* 0xAARRGGBB                        */

/* Opaque handles; GDI+ only ever hands back pointers to these. */
typedef struct CTM_GpGraphics CTM_GpGraphics;
typedef struct CTM_GpPen      CTM_GpPen;
typedef struct CTM_GpBrush    CTM_GpBrush;
typedef struct CTM_GpPath     CTM_GpPath;

/* GDI+ point. Layout matches Win32 POINT (two 32-bit signed values); the
   equality is asserted at compile time in ui.c before any cast is made. */
typedef struct { INT X, Y; } CTM_GpPoint;

typedef struct {
    UINT32  GdiplusVersion;             /* must be 1                         */
    void   *DebugEventCallback;
    BOOL    SuppressBackgroundThread;
    BOOL    SuppressExternalCodecs;
} CTM_GdiplusStartupInput;

/* SmoothingMode */
#define CTM_SmoothingModeAntiAlias      4
/* PixelOffsetMode: Half keeps thin strokes from smearing across two pixels. */
#define CTM_PixelOffsetModeHalf         4
/* Unit */
#define CTM_UnitPixel                   2
/* LineJoin / LineCap */
#define CTM_LineJoinRound               2
#define CTM_LineCapRound                2
/* FillMode */
#define CTM_FillModeAlternate           0

typedef CTM_GpStatus (WINAPI *PFN_GdiplusStartup)(
        ULONG_PTR *token, const CTM_GdiplusStartupInput *input, void *output);
typedef void (WINAPI *PFN_GdiplusShutdown)(ULONG_PTR token);

typedef CTM_GpStatus (WINAPI *PFN_GdipCreateFromHDC)(HDC hdc, CTM_GpGraphics **graphics);
typedef CTM_GpStatus (WINAPI *PFN_GdipDeleteGraphics)(CTM_GpGraphics *graphics);
typedef CTM_GpStatus (WINAPI *PFN_GdipSetSmoothingMode)(CTM_GpGraphics *graphics, INT mode);
typedef CTM_GpStatus (WINAPI *PFN_GdipSetPixelOffsetMode)(CTM_GpGraphics *graphics, INT mode);

typedef CTM_GpStatus (WINAPI *PFN_GdipCreatePen1)(
        CTM_ARGB color, CTM_REAL width, INT unit, CTM_GpPen **pen);
typedef CTM_GpStatus (WINAPI *PFN_GdipDeletePen)(CTM_GpPen *pen);
typedef CTM_GpStatus (WINAPI *PFN_GdipSetPenLineJoin)(CTM_GpPen *pen, INT join);
typedef CTM_GpStatus (WINAPI *PFN_GdipSetPenStartCap)(CTM_GpPen *pen, INT cap);
typedef CTM_GpStatus (WINAPI *PFN_GdipSetPenEndCap)(CTM_GpPen *pen, INT cap);

typedef CTM_GpStatus (WINAPI *PFN_GdipCreateSolidFill)(CTM_ARGB color, CTM_GpBrush **brush);
typedef CTM_GpStatus (WINAPI *PFN_GdipDeleteBrush)(CTM_GpBrush *brush);

typedef CTM_GpStatus (WINAPI *PFN_GdipDrawLinesI)(
        CTM_GpGraphics *graphics, CTM_GpPen *pen, const CTM_GpPoint *points, INT count);
typedef CTM_GpStatus (WINAPI *PFN_GdipFillPolygonI)(
        CTM_GpGraphics *graphics, CTM_GpBrush *brush,
        const CTM_GpPoint *points, INT count, INT fillMode);

/* Path building, for the rounded card outline. GDI+ has no rounded-rectangle
   primitive; the shape is four quarter-circle arcs, which the path joins with
   straight edges on its own. The float variants are used deliberately so the
   outline can sit on a half-pixel inset. */
typedef CTM_GpStatus (WINAPI *PFN_GdipCreatePath)(INT fillMode, CTM_GpPath **path);
typedef CTM_GpStatus (WINAPI *PFN_GdipDeletePath)(CTM_GpPath *path);
typedef CTM_GpStatus (WINAPI *PFN_GdipAddPathArc)(
        CTM_GpPath *path, CTM_REAL x, CTM_REAL y, CTM_REAL width, CTM_REAL height,
        CTM_REAL startAngle, CTM_REAL sweepAngle);
typedef CTM_GpStatus (WINAPI *PFN_GdipClosePathFigure)(CTM_GpPath *path);
typedef CTM_GpStatus (WINAPI *PFN_GdipFillPath)(
        CTM_GpGraphics *graphics, CTM_GpBrush *brush, CTM_GpPath *path);
typedef CTM_GpStatus (WINAPI *PFN_GdipDrawPath)(
        CTM_GpGraphics *graphics, CTM_GpPen *pen, CTM_GpPath *path);

#endif /* CTM_GDIPLUSAPI_H */
