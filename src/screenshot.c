/* ===========================================================================
 * screenshot.c — capture the screen or the focused window to a PNG.
 *
 * Replacing Explorer takes the Snipping Tool's hotkeys with it: PrintScreen is
 * remapped to Snip by a shell setting harden.reg turns off, and Win+Shift+S is
 * a Win chord, which means it belongs to mshell. So without this there is no
 * screenshot at all.
 *
 * WIC rather than GDI+ for the encode. GDI+'s headers are C++-only under
 * mingw-w64, so using it would mean either a C++ translation unit in a C
 * codebase or hand-declaring the flat API; WIC is COM, which C can drive
 * directly through the COBJMACROS wrappers.
 *
 * Files land in %USERPROFILE%\Pictures\Screenshots with a sortable timestamp,
 * which is where Windows itself puts them — so an existing workflow that
 * watches that folder keeps working.
 * =========================================================================== */
#define COBJMACROS
#include "mshell.h"

#include <wincodec.h>
#include <shlobj.h>

/* ---------------------------------------------------------------------------
 * Where to write. Pictures\Screenshots, created if absent.
 * --------------------------------------------------------------------------- */
static bool screenshot_path(wchar_t *out, size_t cap) {
    PWSTR pics = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Pictures, KF_FLAG_CREATE,
                                    NULL, &pics)))
        return false;

    wchar_t dir[MAX_PATH];
    int n = _snwprintf(dir, MAX_PATH, L"%ls\\Screenshots", pics);
    CoTaskMemFree(pics);
    if (n <= 0 || n >= MAX_PATH) return false;

    CreateDirectoryW(dir, NULL);   /* ERROR_ALREADY_EXISTS is the normal case */

    SYSTEMTIME t;
    GetLocalTime(&t);
    n = _snwprintf(out, cap, L"%ls\\mshell-%04u%02u%02u-%02u%02u%02u.png",
                   dir, t.wYear, t.wMonth, t.wDay,
                   t.wHour, t.wMinute, t.wSecond);
    return n > 0 && (size_t)n < cap;
}

/* ---------------------------------------------------------------------------
 * Encode a 32-bit top-down BGRA buffer to a PNG file.
 * --------------------------------------------------------------------------- */
static bool write_png(const wchar_t *path, const BYTE *pixels,
                      UINT w, UINT h, UINT stride) {
    IWICImagingFactory  *factory = NULL;
    IWICBitmapEncoder   *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IWICStream          *stream  = NULL;
    bool ok = false;

    /* Apartment-threaded and per-call: mshell has no COM apartment of its own,
     * and CoInitializeEx is refcounted, so this neither disturbs anything nor
     * leaves an apartment behind. RPC_E_CHANGED_MODE means somebody already
     * chose a different model, which is fine — we just must not uninitialise. */
    HRESULT hr_init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool    did_init = SUCCEEDED(hr_init);

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL,
                                CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                (void **)&factory)))
        goto out;

    if (FAILED(IWICImagingFactory_CreateStream(factory, &stream))) goto out;
    if (FAILED(IWICStream_InitializeFromFilename(stream, path, GENERIC_WRITE)))
        goto out;
    if (FAILED(IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng,
                                                NULL, &encoder)))
        goto out;
    if (FAILED(IWICBitmapEncoder_Initialize(encoder, (IStream *)stream,
                                            WICBitmapEncoderNoCache)))
        goto out;
    if (FAILED(IWICBitmapEncoder_CreateNewFrame(encoder, &frame, NULL))) goto out;
    if (FAILED(IWICBitmapFrameEncode_Initialize(frame, NULL)))            goto out;
    if (FAILED(IWICBitmapFrameEncode_SetSize(frame, w, h)))               goto out;

    /* 32bppBGRA is exactly what a DIB section gives us, so the encoder does the
     * conversion rather than us walking the buffer. */
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(IWICBitmapFrameEncode_SetPixelFormat(frame, &fmt)))        goto out;

    if (FAILED(IWICBitmapFrameEncode_WritePixels(frame, h, stride,
                                                 stride * h, (BYTE *)pixels)))
        goto out;
    if (FAILED(IWICBitmapFrameEncode_Commit(frame)))   goto out;
    if (FAILED(IWICBitmapEncoder_Commit(encoder)))     goto out;
    ok = true;

out:
    if (frame)   IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream)  IWICStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory);
    if (did_init) CoUninitialize();
    return ok;
}

/* ---------------------------------------------------------------------------
 * Capture `src` into a PNG. Also leaves the bitmap on the clipboard, because
 * pasting straight into a chat window is what a screenshot is usually for and
 * mshell has no gallery to browse.
 * --------------------------------------------------------------------------- */
static void capture_rect(RECT src, const wchar_t *what) {
    int w = src.right - src.left, h = src.bottom - src.top;
    if (w <= 0 || h <= 0) {
        log_err(L"screenshot: %ls has no area to capture", what);
        return;
    }

    HDC screen = GetDC(NULL);
    HDC mem    = CreateCompatibleDC(screen);

    /* A DIB section rather than a compatible bitmap: we need the pixels back,
     * and this hands us a pointer instead of needing GetDIBits. Negative height
     * makes it top-down, which is the order WIC wants. */
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void   *bits = NULL;
    HBITMAP dib  = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib || !bits) {
        log_err(L"screenshot: CreateDIBSection failed: %lu", GetLastError());
        if (dib) DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
        return;
    }

    HBITMAP old = (HBITMAP)SelectObject(mem, dib);

    /* CAPTUREBLT so layered windows are included — without it our own overlays,
     * and any translucent app window, come out as holes. */
    if (!BitBlt(mem, 0, 0, w, h, screen, src.left, src.top, SRCCOPY | CAPTUREBLT))
        log_msg(LOG_WARN, L"screenshot: BitBlt failed: %lu", GetLastError());

    SelectObject(mem, old);

    /* --- clipboard ---
     * Done before the file write so a failure to write still leaves something
     * pasteable. The clipboard takes ownership of the copy, hence the extra
     * CopyImage rather than handing it our DIB. */
    HBITMAP clip = (HBITMAP)CopyImage(dib, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    if (clip && OpenClipboard(NULL)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_BITMAP, clip)) DeleteObject(clip);
        CloseClipboard();
    } else if (clip) {
        DeleteObject(clip);
    }

    wchar_t path[MAX_PATH];
    if (screenshot_path(path, MAX_PATH) &&
        write_png(path, (const BYTE *)bits, (UINT)w, (UINT)h, (UINT)(w * 4))) {
        log_msg(LOG_INFO, L"screenshot: %ls -> %ls", what, path);
    } else {
        log_err(L"screenshot: captured %ls but could not write the PNG "
                L"(it is still on the clipboard)", what);
    }

    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
}

/* The whole virtual screen — every monitor, in their arranged positions. */
void screenshot_screen(void) {
    RECT r = { GetSystemMetrics(SM_XVIRTUALSCREEN),
               GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0 };
    r.right  = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    capture_rect(r, L"screen");
}

/* Just the focused window, at its DWM visible frame — the same rect the tiler
 * and the focus ring use, so the capture matches what the ring was drawn round
 * rather than including the invisible resize border. */
void screenshot_window(void) {
    HWND focus = desktop_get_focused();
    RECT r;
    if (!focus || !window_frame_rect(focus, &r)) {
        log_err(L"screenshot: no focused window to capture");
        return;
    }
    capture_rect(r, L"window");
}
