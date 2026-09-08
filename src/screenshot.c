#define COBJMACROS
#include "mshell.h"

#include <wincodec.h>
#include <shlobj.h>

static bool screenshot_path(wchar_t *out, size_t cap) {
    PWSTR pics = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Pictures, KF_FLAG_CREATE,
                                    NULL, &pics)))
        return false;

    wchar_t dir[MAX_PATH];
    int n = _snwprintf(dir, MAX_PATH, L"%ls\\Screenshots", pics);
    CoTaskMemFree(pics);
    if (n <= 0 || n >= MAX_PATH) return false;

    CreateDirectoryW(dir, NULL);

    SYSTEMTIME t;
    GetLocalTime(&t);
    n = _snwprintf(out, cap, L"%ls\\mshell-%04u%02u%02u-%02u%02u%02u.png",
                   dir, t.wYear, t.wMonth, t.wDay,
                   t.wHour, t.wMinute, t.wSecond);
    return n > 0 && (size_t)n < cap;
}

static bool write_png(const wchar_t *path, const BYTE *pixels,
                      UINT w, UINT h, UINT stride) {
    IWICImagingFactory  *factory = NULL;
    IWICBitmapEncoder   *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IWICStream          *stream  = NULL;
    bool ok = false;

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

static void capture_rect(RECT src, const wchar_t *what) {
    int w = src.right - src.left, h = src.bottom - src.top;
    if (w <= 0 || h <= 0) {
        log_err(L"screenshot: %ls has no area to capture", what);
        return;
    }

    HDC screen = GetDC(NULL);
    HDC mem    = CreateCompatibleDC(screen);

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

    if (!BitBlt(mem, 0, 0, w, h, screen, src.left, src.top, SRCCOPY | CAPTUREBLT))
        log_msg(LOG_WARN, L"screenshot: BitBlt failed: %lu", GetLastError());

    SelectObject(mem, old);

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

void screenshot_screen(void) {
    RECT r = { GetSystemMetrics(SM_XVIRTUALSCREEN),
               GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0 };
    r.right  = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    capture_rect(r, L"screen");
}

void screenshot_window(void) {
    HWND focus = desktop_get_focused();
    RECT r;
    if (!focus || !window_frame_rect(focus, &r)) {
        log_err(L"screenshot: no focused window to capture");
        return;
    }
    capture_rect(r, L"window");
}
