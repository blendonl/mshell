#ifndef MSHELL_OVERLAY_H
#define MSHELL_OVERLAY_H

#include <windows.h>
#include <stdbool.h>

bool overlay_register(const wchar_t *cls, WNDPROC proc, bool arrow_cursor);

HWND overlay_create(const wchar_t *cls, DWORD exstyle);

void overlay_destroy(HWND *hwnd, const wchar_t *cls);

void overlay_raise_all(void);

int overlay_scale(int px, UINT dpi);

typedef struct {
    HFONT   font;
    UINT    dpi;
    int     px;
    wchar_t face[LF_FACESIZE];
} OverlayFont;

HFONT overlay_font(OverlayFont *of, UINT dpi, int px);

HFONT overlay_font_face(OverlayFont *of, UINT dpi, int px, const wchar_t *face);

void  overlay_font_free(OverlayFont *of);

typedef struct {
    HWND        hwnd;
    PAINTSTRUCT ps;
    HDC         hdc;
    HDC         mem;
    HBITMAP     bmp;
    HBITMAP     obm;
    int         w, h;
} OverlayPaint;

HDC  overlay_paint_begin(OverlayPaint *p, HWND hwnd);

void overlay_paint_end(OverlayPaint *p);

void overlay_fill(HDC dc, const RECT *rc, COLORREF color);

#endif
