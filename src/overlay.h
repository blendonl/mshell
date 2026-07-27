/* ===========================================================================
 * overlay.h — the shared parts of mshell's own on-screen windows.
 *
 * mshell paints four surfaces of its own (backdrop, focus ring, which-key
 * panel, status bar) and is about to paint more. They differ entirely in what
 * they draw and agree entirely on how they exist: a zero-geometry WS_POPUP of a
 * private class, never activated, positioned only through SetWindowPos, erasing
 * nothing and painting the whole client area itself.
 *
 * Three things had been copied rather than shared, and this is where they live
 * now:
 *
 *   - DPI scaling. wk_dpi_scale() and bar_scale() were the same MulDiv.
 *   - The DPI-aware font cache. Both rebuilt a font whenever the monitor's DPI
 *     changed and both tracked "which DPI is this font for" by hand.
 *   - Double-buffered paint. Both open with CreateCompatibleDC/Bitmap and close
 *     with a BitBlt, and getting the object-restore order wrong leaks GDI
 *     handles quietly.
 *
 * What deliberately stays with each overlay is its window procedure and its
 * drawing: they have nothing in common beyond the above, and folding them
 * together would mean a switch on "which overlay am I" inside shared code.
 * =========================================================================== */
#ifndef MSHELL_OVERLAY_H
#define MSHELL_OVERLAY_H

#include <windows.h>
#include <stdbool.h>

/* --- class + window ---------------------------------------------------- */

/* Register an overlay class. Idempotent: re-registering the same name is a
 * no-op, which is what lets the status bar register once and create a window
 * per monitor. `arrow_cursor` matters only for a surface the pointer can rest
 * over (the backdrop); leave it false and the cursor is whatever the previous
 * window set. */
bool overlay_register(const wchar_t *cls, WNDPROC proc, bool arrow_cursor);

/* Create a hidden, zero-geometry WS_POPUP of a registered class. Geometry
 * always arrives later via SetWindowPos, so there is no size argument. Logs and
 * returns NULL on failure — every caller treats an overlay as best-effort. */
HWND overlay_create(const wchar_t *cls, DWORD exstyle);

/* DestroyWindow + NULL the caller's handle + unregister the class. Safe on a
 * NULL handle so shutdown paths need no guard. */
void overlay_destroy(HWND *hwnd, const wchar_t *cls);

/* --- DPI ---------------------------------------------------------------- */

/* Scale a design pixel measured at 96 DPI. mshell is per-monitor DPI aware, so
 * nothing scales these for us. */
int overlay_scale(int px, UINT dpi);

/* --- fonts -------------------------------------------------------------- */

/* A font plus the DPI and pixel height it was built for, so a rebuild happens
 * only when one of those actually changed. Zero-initialise it. */
typedef struct {
    HFONT font;
    UINT  dpi;
    int   px;
} OverlayFont;

/* Segoe UI at `px` pixels (a negative height is applied internally, so pass a
 * positive number). Returns the cached font when dpi and px are unchanged. */
HFONT overlay_font(OverlayFont *of, UINT dpi, int px);
void  overlay_font_free(OverlayFont *of);

/* --- painting ----------------------------------------------------------- */

typedef struct {
    HWND        hwnd;
    PAINTSTRUCT ps;
    HDC         hdc;    /* the real window DC */
    HDC         mem;    /* draw into this one */
    HBITMAP     bmp;
    HBITMAP     obm;
    int         w, h;   /* client size */
} OverlayPaint;

/* BeginPaint + a memory DC sized to the client area. Returns the DC to draw
 * into, or NULL if the window has no area yet (in which case EndPaint has
 * already run and the caller should just return). */
HDC  overlay_paint_begin(OverlayPaint *p, HWND hwnd);

/* BitBlt the buffer to screen, restore and free every GDI object, EndPaint. */
void overlay_paint_end(OverlayPaint *p);

/* CreateSolidBrush + FillRect + DeleteObject, which appeared verbatim in all
 * four overlays. */
void overlay_fill(HDC dc, const RECT *rc, COLORREF color);

#endif /* MSHELL_OVERLAY_H */
