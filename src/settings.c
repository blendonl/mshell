#include "mshell.h"
#include "match.h"
#include "settings_catalog.h"
#include "settings_parse.h"

#include <roapi.h>
#include <winstring.h>

#define SETTING_KEY_ROOT     L"SOFTWARE\\Microsoft\\SystemSettings\\SettingId"
#define SETTING_TIMEOUT_MS   15000
#define SETTING_UPDATING_MS  3000
#define SETTING_REPORT_CAP   1024
#define SETTING_MAX_OPTIONS  64
#define SETTING_OUT_CHUNK    32000

static const IID IID_ISettingItem =
    { 0x40C037CC, 0xD8BF, 0x489E, { 0x86, 0x97, 0xD6, 0x6B, 0xAA, 0x32, 0x21, 0xBF } };
static const IID IID_IPropertyValue =
    { 0x4BD682DD, 0x7554, 0x40E9, { 0x9A, 0x9B, 0x82, 0x65, 0x4E, 0xDE, 0x7E, 0x62 } };
static const IID IID_IPropertyValueStatics =
    { 0x629BDBC8, 0xD932, 0x4FF4, { 0x96, 0xB9, 0x8D, 0x96, 0xC5, 0xC1, 0xE8, 0x58 } };
static const IID IID_IVectorOfInspectable =
    { 0xB32BDCA4, 0x5E52, 0x5B27, { 0xBC, 0x5D, 0xD6, 0x6A, 0x1A, 0x26, 0x8C, 0x2A } };
static const IID IID_IVectorViewOfInspectable =
    { 0xA6487363, 0xB074, 0x5C60, { 0xAB, 0x16, 0x86, 0x6D, 0xCE, 0xE4, 0xEE, 0x54 } };

enum {
    SLOT_QUERY_INTERFACE = 0,
    SLOT_RELEASE         = 2,
    SLOT_GET_IIDS        = 3,
};

enum {
    SLOT_ITEM_ID         = 6,
    SLOT_ITEM_TYPE       = 7,
    SLOT_ITEM_POLICY     = 8,
    SLOT_ITEM_ENABLED    = 9,
    SLOT_ITEM_UPDATING   = 12,
    SLOT_ITEM_GET_VALUE  = 13,
    SLOT_ITEM_SET_VALUE  = 14,
};

enum {
    SLOT_PV_TYPE    = 6,
    SLOT_PV_UINT8   = 8,
    SLOT_PV_INT16   = 9,
    SLOT_PV_UINT16  = 10,
    SLOT_PV_INT32   = 11,
    SLOT_PV_UINT32  = 12,
    SLOT_PV_INT64   = 13,
    SLOT_PV_UINT64  = 14,
    SLOT_PV_SINGLE  = 15,
    SLOT_PV_DOUBLE  = 16,
    SLOT_PV_BOOLEAN = 18,
    SLOT_PV_STRING  = 19,
};

enum {
    SLOT_PVS_UINT8   = 7,
    SLOT_PVS_INT16   = 8,
    SLOT_PVS_UINT16  = 9,
    SLOT_PVS_INT32   = 10,
    SLOT_PVS_UINT32  = 11,
    SLOT_PVS_INT64   = 12,
    SLOT_PVS_UINT64  = 13,
    SLOT_PVS_SINGLE  = 14,
    SLOT_PVS_DOUBLE  = 15,
    SLOT_PVS_BOOLEAN = 17,
    SLOT_PVS_STRING  = 18,
};

enum {
    SLOT_VECTOR_GET_AT = 6,
    SLOT_VECTOR_SIZE   = 7,
};

typedef enum {
    PT_UINT8 = 1, PT_INT16, PT_UINT16, PT_INT32, PT_UINT32, PT_INT64,
    PT_UINT64, PT_SINGLE, PT_DOUBLE, PT_CHAR16, PT_BOOLEAN, PT_STRING,
} PropertyTypeCode;

typedef HRESULT (*QueryInterfaceFn)(void *, const IID *, void **);
typedef ULONG   (*ReleaseFn)(void *);
typedef HRESULT (*GetIidsFn)(void *, ULONG *, IID **);
typedef HRESULT (*GetHstringFn)(void *, HSTRING *);
typedef HRESULT (*GetInt32Fn)(void *, INT32 *);
typedef HRESULT (*GetUInt32Fn)(void *, UINT32 *);
typedef HRESULT (*GetBooleanFn)(void *, BOOLEAN *);
typedef HRESULT (*GetUInt8Fn)(void *, BYTE *);
typedef HRESULT (*GetInt16Fn)(void *, INT16 *);
typedef HRESULT (*GetUInt16Fn)(void *, UINT16 *);
typedef HRESULT (*GetInt64Fn)(void *, INT64 *);
typedef HRESULT (*GetUInt64Fn)(void *, UINT64 *);
typedef HRESULT (*GetSingleFn)(void *, FLOAT *);
typedef HRESULT (*GetDoubleFn)(void *, DOUBLE *);
typedef HRESULT (*GetNamedFn)(void *, HSTRING, void **);
typedef HRESULT (*SetNamedFn)(void *, HSTRING, void *);
typedef HRESULT (*GetAtFn)(void *, UINT32, void **);
typedef HRESULT (*CreateUInt8Fn)(void *, BYTE, void **);
typedef HRESULT (*CreateInt16Fn)(void *, INT16, void **);
typedef HRESULT (*CreateUInt16Fn)(void *, UINT16, void **);
typedef HRESULT (*CreateInt32Fn)(void *, INT32, void **);
typedef HRESULT (*CreateUInt32Fn)(void *, UINT32, void **);
typedef HRESULT (*CreateInt64Fn)(void *, INT64, void **);
typedef HRESULT (*CreateUInt64Fn)(void *, UINT64, void **);
typedef HRESULT (*CreateSingleFn)(void *, FLOAT, void **);
typedef HRESULT (*CreateDoubleFn)(void *, DOUBLE, void **);
typedef HRESULT (*CreateBooleanFn)(void *, BOOLEAN, void **);
typedef HRESULT (*CreateStringFn)(void *, HSTRING, void **);
typedef HRESULT (*GetSettingFn)(HSTRING, void **);

#define VCALL(obj, slot, type, ...) (((type)vslot((obj), (slot)))((obj), __VA_ARGS__))

typedef struct {
    SettingValue value;
    int          ptype;
} ReadValue;

typedef struct {
    wchar_t text[SETTING_REPORT_CAP];
    size_t  len;
    bool    failed;
    bool    disabled;
} Report;

typedef struct {
    const SettingField *field;
    bool                set;
    wchar_t             name[96];
    wchar_t             want[SETTING_TEXT_MAX];
    Report              report;
} SettingJob;

static void *vslot(void *obj, int index) {
    return (*(void ***)obj)[index];
}

static void com_release(void *obj) {
    if (obj) ((ReleaseFn)vslot(obj, SLOT_RELEASE))(obj);
}

static HSTRING hstring_of(const wchar_t *s) {
    HSTRING h = NULL;
    if (FAILED(WindowsCreateString(s, (UINT32)wcslen(s), &h))) return NULL;
    return h;
}

static void report_line(Report *r, const wchar_t *fmt, ...) {
    if (r->len + 3 >= SETTING_REPORT_CAP) return;
    if (r->len > 0) {
        r->text[r->len++] = L'\r';
        r->text[r->len++] = L'\n';
    }
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(r->text + r->len, SETTING_REPORT_CAP - r->len - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)wcsnlen(r->text + r->len, SETTING_REPORT_CAP - r->len - 1);
    r->len += (size_t)n;
    r->text[r->len] = L'\0';
}

static void report_fail(Report *r, const wchar_t *name, const wchar_t *fmt, ...) {
    wchar_t why[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(why, ARRAYSIZE(why) - 1, fmt, ap);
    va_end(ap);
    why[ARRAYSIZE(why) - 1] = L'\0';
    report_line(r, L"%ls: FAILED — %ls", name, why);
    r->failed = true;
}

static void write_utf8(const char *u8) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) {
        DWORD written = 0;
        WriteFile(out, u8, (DWORD)strlen(u8), &written, NULL);
        WriteFile(out, "\r\n", 2, &written, NULL);
        return;
    }
    console_print(u8);
}

static void print_wide(const wchar_t *text) {
    int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (need <= 0) return;
    char *u8 = (char *)malloc((size_t)need);
    if (!u8) return;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, u8, need, NULL, NULL);
    write_utf8(u8);
    free(u8);
}

static bool handler_dll(const wchar_t *id, wchar_t *dll, DWORD dll_cap) {
    wchar_t key[512];
    if (_snwprintf(key, ARRAYSIZE(key), SETTING_KEY_ROOT L"\\%ls", id) < 0) return false;
    DWORD size = dll_cap * sizeof(wchar_t);
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, L"DllPath",
                        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, dll, &size) == ERROR_SUCCESS;
}

static bool implements_setting_item(void *item) {
    ULONG count = 0;
    IID  *iids  = NULL;
    if (FAILED(VCALL(item, SLOT_GET_IIDS, GetIidsFn, &count, &iids))) return false;
    bool found = false;
    for (ULONG k = 0; k < count; k++)
        if (IsEqualIID(&iids[k], &IID_ISettingItem)) found = true;
    CoTaskMemFree(iids);
    return found;
}

static void *open_item(const wchar_t *id, wchar_t *why, size_t why_cap) {
    wchar_t dll[MAX_PATH];
    if (!handler_dll(id, dll, MAX_PATH)) {
        _snwprintf(why, why_cap, L"this Windows build does not have it");
        return NULL;
    }

    HMODULE mod = LoadLibraryExW(dll, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!mod) {
        _snwprintf(why, why_cap, L"could not load %ls (error %lu)", dll, GetLastError());
        return NULL;
    }
    GetSettingFn get_setting = (GetSettingFn)(void *)GetProcAddress(mod, "GetSetting");
    if (!get_setting) {
        _snwprintf(why, why_cap, L"%ls has no GetSetting export", dll);
        return NULL;
    }

    HSTRING hid = hstring_of(id);
    void   *item = NULL;
    HRESULT hr = hid ? get_setting(hid, &item) : E_OUTOFMEMORY;
    WindowsDeleteString(hid);
    if (FAILED(hr) || !item) {
        _snwprintf(why, why_cap, L"Windows would not open it (0x%08lX)", (unsigned long)hr);
        return NULL;
    }

    if (!implements_setting_item(item)) {
        _snwprintf(why, why_cap, L"Windows returned an object mshell does not recognise");
        com_release(item);
        return NULL;
    }

    HSTRING got = NULL;
    VCALL(item, SLOT_ITEM_ID, GetHstringFn, &got);
    bool same = got && _wcsicmp(WindowsGetStringRawBuffer(got, NULL), id) == 0;
    WindowsDeleteString(got);
    if (!same) {
        _snwprintf(why, why_cap, L"Windows answered for a different setting");
        com_release(item);
        return NULL;
    }
    return item;
}

static void pump_messages(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
}

static void wait_not_updating(void *item) {
    ULONGLONG until = GetTickCount64() + SETTING_UPDATING_MS;
    BOOLEAN updating = 0;
    while (SUCCEEDED(VCALL(item, SLOT_ITEM_UPDATING, GetBooleanFn, &updating)) && updating &&
           GetTickCount64() < until) {
        pump_messages();
        Sleep(20);
    }
}

static void pump_for(DWORD ms) {
    ULONGLONG until = GetTickCount64() + ms;
    while (GetTickCount64() < until) {
        pump_messages();
        Sleep(20);
    }
}

static bool unbox(void *inspectable, ReadValue *out) {
    memset(out, 0, sizeof *out);
    void *pv = NULL;
    if (FAILED(VCALL(inspectable, SLOT_QUERY_INTERFACE, QueryInterfaceFn,
                     &IID_IPropertyValue, &pv)) || !pv)
        return false;

    int ptype = 0;
    VCALL(pv, SLOT_PV_TYPE, GetInt32Fn, &ptype);
    out->ptype = ptype;
    SettingValue *v = &out->value;
    bool ok = true;

    switch (ptype) {
    case PT_UINT8:   { BYTE b = 0;    ok = SUCCEEDED(VCALL(pv, SLOT_PV_UINT8,  GetUInt8Fn,  &b)); v->kind = SV_UINT32; v->u = b; break; }
    case PT_UINT16:  { UINT16 w = 0;  ok = SUCCEEDED(VCALL(pv, SLOT_PV_UINT16, GetUInt16Fn, &w)); v->kind = SV_UINT32; v->u = w; break; }
    case PT_UINT32:  { UINT32 d = 0;  ok = SUCCEEDED(VCALL(pv, SLOT_PV_UINT32, GetUInt32Fn, &d)); v->kind = SV_UINT32; v->u = d; break; }
    case PT_INT16:   { INT16 s = 0;   ok = SUCCEEDED(VCALL(pv, SLOT_PV_INT16,  GetInt16Fn,  &s)); v->kind = SV_INT32;  v->i = s; break; }
    case PT_INT32:   { INT32 s = 0;   ok = SUCCEEDED(VCALL(pv, SLOT_PV_INT32,  GetInt32Fn,  &s)); v->kind = SV_INT32;  v->i = s; break; }
    case PT_INT64:   { INT64 s = 0;   ok = SUCCEEDED(VCALL(pv, SLOT_PV_INT64,  GetInt64Fn,  &s)); v->kind = SV_INT64;  v->i = s; break; }
    case PT_UINT64:  { UINT64 d = 0;  ok = SUCCEEDED(VCALL(pv, SLOT_PV_UINT64, GetUInt64Fn, &d)); v->kind = SV_UINT64; v->u = d; break; }
    case PT_SINGLE:  { FLOAT f = 0;   ok = SUCCEEDED(VCALL(pv, SLOT_PV_SINGLE, GetSingleFn, &f)); v->kind = SV_DOUBLE; v->d = f; break; }
    case PT_DOUBLE:  { DOUBLE f = 0;  ok = SUCCEEDED(VCALL(pv, SLOT_PV_DOUBLE, GetDoubleFn, &f)); v->kind = SV_DOUBLE; v->d = f; break; }
    case PT_BOOLEAN: { BOOLEAN b = 0; ok = SUCCEEDED(VCALL(pv, SLOT_PV_BOOLEAN, GetBooleanFn, &b)); v->kind = SV_BOOL; v->b = b != 0; break; }
    case PT_STRING: {
        HSTRING s = NULL;
        ok = SUCCEEDED(VCALL(pv, SLOT_PV_STRING, GetHstringFn, &s));
        v->kind = SV_STRING;
        _snwprintf(v->s, SETTING_TEXT_MAX - 1, L"%ls", s ? WindowsGetStringRawBuffer(s, NULL) : L"");
        v->s[SETTING_TEXT_MAX - 1] = L'\0';
        WindowsDeleteString(s);
        break;
    }
    default:
        ok = false;
        break;
    }
    com_release(pv);
    return ok;
}

static HRESULT get_named(void *item, const wchar_t *name, void **out) {
    *out = NULL;
    HSTRING h = hstring_of(name);
    if (!h) return E_OUTOFMEMORY;
    HRESULT hr = VCALL(item, SLOT_ITEM_GET_VALUE, GetNamedFn, h, out);
    WindowsDeleteString(h);
    if (SUCCEEDED(hr) && !*out) hr = E_POINTER;
    return hr;
}

static bool read_named(void *item, const wchar_t *name, ReadValue *out) {
    void *boxed = NULL;
    if (FAILED(get_named(item, name, &boxed))) return false;
    bool ok = unbox(boxed, out);
    com_release(boxed);
    return ok;
}

static int read_options(void *item, wchar_t options[][SETTING_TEXT_MAX], int cap) {
    void *boxed = NULL;
    if (FAILED(get_named(item, L"PossibleValues", &boxed))) return -1;

    void *vec = NULL;
    if (FAILED(VCALL(boxed, SLOT_QUERY_INTERFACE, QueryInterfaceFn,
                     &IID_IVectorOfInspectable, &vec)) &&
        FAILED(VCALL(boxed, SLOT_QUERY_INTERFACE, QueryInterfaceFn,
                     &IID_IVectorViewOfInspectable, &vec))) {
        com_release(boxed);
        return -1;
    }

    UINT32 size = 0;
    VCALL(vec, SLOT_VECTOR_SIZE, GetUInt32Fn, &size);
    int count = 0;
    for (UINT32 k = 0; k < size && count < cap; k++) {
        void *entry = NULL;
        ReadValue rv;
        if (FAILED(VCALL(vec, SLOT_VECTOR_GET_AT, GetAtFn, k, &entry)) || !entry) continue;
        if (unbox(entry, &rv) && rv.value.kind == SV_STRING)
            wcscpy(options[count++], rv.value.s);
        com_release(entry);
    }
    com_release(vec);
    com_release(boxed);
    return count;
}

static void *box(const SettingValue *v, int ptype, wchar_t *why, size_t why_cap) {
    void *statics = NULL;
    HSTRING_HEADER header;
    HSTRING cls = NULL;
    WindowsCreateStringReference(L"Windows.Foundation.PropertyValue", 32, &header, &cls);
    HRESULT hr = RoGetActivationFactory(cls, &IID_IPropertyValueStatics, &statics);
    if (FAILED(hr) || !statics) {
        _snwprintf(why, why_cap, L"Windows would not create a value (0x%08lX)", (unsigned long)hr);
        return NULL;
    }

    void *out = NULL;
    bool  fits = true;
    switch (ptype) {
    case PT_UINT8:   fits = v->u <= 0xFF;   if (fits) hr = VCALL(statics, SLOT_PVS_UINT8,  CreateUInt8Fn,  (BYTE)v->u, &out);   break;
    case PT_UINT16:  fits = v->u <= 0xFFFF; if (fits) hr = VCALL(statics, SLOT_PVS_UINT16, CreateUInt16Fn, (UINT16)v->u, &out); break;
    case PT_UINT32:  hr = VCALL(statics, SLOT_PVS_UINT32, CreateUInt32Fn, (UINT32)v->u, &out); break;
    case PT_INT16:   fits = v->i >= -32768 && v->i <= 32767; if (fits) hr = VCALL(statics, SLOT_PVS_INT16, CreateInt16Fn, (INT16)v->i, &out); break;
    case PT_INT32:   hr = VCALL(statics, SLOT_PVS_INT32,   CreateInt32Fn,   (INT32)v->i, &out); break;
    case PT_INT64:   hr = VCALL(statics, SLOT_PVS_INT64,   CreateInt64Fn,   (INT64)v->i, &out); break;
    case PT_UINT64:  hr = VCALL(statics, SLOT_PVS_UINT64,  CreateUInt64Fn,  (UINT64)v->u, &out); break;
    case PT_SINGLE:  hr = VCALL(statics, SLOT_PVS_SINGLE,  CreateSingleFn,  (FLOAT)v->d, &out); break;
    case PT_DOUBLE:  hr = VCALL(statics, SLOT_PVS_DOUBLE,  CreateDoubleFn,  (DOUBLE)v->d, &out); break;
    case PT_BOOLEAN: hr = VCALL(statics, SLOT_PVS_BOOLEAN, CreateBooleanFn, (BOOLEAN)v->b, &out); break;
    case PT_STRING: {
        HSTRING s = hstring_of(v->s);
        hr = s ? VCALL(statics, SLOT_PVS_STRING, CreateStringFn, s, &out) : E_OUTOFMEMORY;
        WindowsDeleteString(s);
        break;
    }
    default:
        hr = E_NOTIMPL;
        break;
    }
    com_release(statics);

    if (!fits) {
        _snwprintf(why, why_cap, L"the value is too large for this setting");
        return NULL;
    }
    if (FAILED(hr) || !out) {
        _snwprintf(why, why_cap, L"Windows would not create a value of this type (0x%08lX)",
                   (unsigned long)hr);
        com_release(out);
        return NULL;
    }
    return out;
}

static void word_of(const SettingField *f, const SettingValue *raw, wchar_t *out, size_t cap) {
    char word[SETTING_TEXT_MAX];
    if (settings_field_word_of(f, raw, word, sizeof word)) {
        _snwprintf(out, cap - 1, L"%hs", word);
        out[cap - 1] = L'\0';
        return;
    }
    wchar_t shown[SETTING_TEXT_MAX];
    settings_format_value(raw, shown, ARRAYSIZE(shown));
    _snwprintf(out, cap - 1, L"%ls (not a value mshell names)", shown);
    out[cap - 1] = L'\0';
}

static void job_get(SettingJob *job, void *item) {
    Report *r = &job->report;

    ReadValue current;
    if (!read_named(item, L"Value", &current)) {
        report_fail(r, job->name, L"its value could not be read");
        return;
    }

    wchar_t shown[SETTING_TEXT_MAX + 32];
    word_of(job->field, &current.value, shown, ARRAYSIZE(shown));

    BOOLEAN policy = 0, enabled = 1;
    VCALL(item, SLOT_ITEM_POLICY, GetBooleanFn, &policy);
    VCALL(item, SLOT_ITEM_ENABLED, GetBooleanFn, &enabled);
    report_line(r, L"%ls = %ls%ls%ls", job->name, shown,
                policy ? L"  (set by group policy)" : L"",
                !enabled ? L"  (disabled right now)" : L"");
}

static bool want_value(SettingJob *job, void *item, const ReadValue *current, SettingValue *want) {
    Report *r = &job->report;

    if (current->value.kind == SV_STRING) {
        wchar_t options[SETTING_MAX_OPTIONS][SETTING_TEXT_MAX];
        int count = read_options(item, options, SETTING_MAX_OPTIONS);
        for (int k = 0; k < count; k++) {
            if (wcscmp(options[k], job->want) != 0) continue;
            memset(want, 0, sizeof *want);
            want->kind = SV_STRING;
            wcscpy(want->s, options[k]);
            return true;
        }
        report_fail(r, job->name, L"Windows no longer offers that choice here");
        return false;
    }

    const char *why = NULL;
    if (!settings_parse_value(current->value.kind, job->want, want, &why)) {
        report_fail(r, job->name, L"Windows keeps this as a different kind of value (%hs)",
                    why ? why : "invalid");
        return false;
    }

    ReadValue lo, hi;
    bool have_lo = read_named(item, L"MinValue", &lo);
    bool have_hi = read_named(item, L"MaxValue", &hi);
    if (!settings_value_in_range(want, have_lo ? &lo.value : NULL, have_hi ? &hi.value : NULL)) {
        wchar_t lo_t[64] = L"?", hi_t[64] = L"?";
        if (have_lo) settings_format_value(&lo.value, lo_t, ARRAYSIZE(lo_t));
        if (have_hi) settings_format_value(&hi.value, hi_t, ARRAYSIZE(hi_t));
        report_fail(r, job->name, L"this Windows only accepts %ls .. %ls", lo_t, hi_t);
        return false;
    }
    return true;
}

static void job_set(SettingJob *job, void *item) {
    Report *r = &job->report;

    BOOLEAN policy = 0, enabled = 1;
    VCALL(item, SLOT_ITEM_POLICY, GetBooleanFn, &policy);
    VCALL(item, SLOT_ITEM_ENABLED, GetBooleanFn, &enabled);
    if (policy) {
        report_fail(r, job->name, L"it is set by group policy");
        return;
    }
    if (!enabled) {
        report_fail(r, job->name, L"it is disabled right now, usually by another setting");
        r->disabled = true;
        return;
    }

    ReadValue current;
    if (!read_named(item, L"Value", &current)) {
        report_fail(r, job->name, L"its current value could not be read");
        return;
    }

    SettingValue want;
    if (!want_value(job, item, &current, &want)) return;

    wchar_t before[SETTING_TEXT_MAX + 32], after[SETTING_TEXT_MAX + 32];
    word_of(job->field, &current.value, before, ARRAYSIZE(before));
    word_of(job->field, &want, after, ARRAYSIZE(after));

    if (settings_value_equal(&current.value, &want)) {
        report_line(r, L"%ls: already %ls", job->name, after);
        return;
    }

    wchar_t why[256];
    void *boxed = box(&want, current.ptype, why, ARRAYSIZE(why));
    if (!boxed) {
        report_fail(r, job->name, L"%ls", why);
        return;
    }

    HSTRING name = hstring_of(L"Value");
    HRESULT hr = name ? VCALL(item, SLOT_ITEM_SET_VALUE, SetNamedFn, name, boxed) : E_OUTOFMEMORY;
    WindowsDeleteString(name);
    com_release(boxed);
    if (FAILED(hr)) {
        report_fail(r, job->name, L"Windows refused the change (0x%08lX)", (unsigned long)hr);
        return;
    }

    pump_for(200);
    wait_not_updating(item);

    ReadValue now;
    if (read_named(item, L"Value", &now) && !settings_value_equal(&now.value, &want)) {
        wchar_t now_t[SETTING_TEXT_MAX + 32];
        word_of(job->field, &now.value, now_t, ARRAYSIZE(now_t));
        report_fail(r, job->name, L"Windows accepted %ls but it still reads %ls", after, now_t);
        return;
    }
    report_line(r, L"%ls: %ls -> %ls", job->name, before, after);
}

static void job_run(SettingJob *job) {
    wchar_t why[512];
    void *item = open_item(job->field->id, why, ARRAYSIZE(why));
    if (!item) {
        report_fail(&job->report, job->name, L"%ls", why);
        return;
    }

    int type = SETTING_UNKNOWN;
    VCALL(item, SLOT_ITEM_TYPE, GetInt32Fn, &type);
    if (!settings_type_writable((SettingType)type)) {
        report_fail(&job->report, job->name, L"Windows now keeps it as a %ls, which has no value",
                    settings_type_name((SettingType)type));
        com_release(item);
        return;
    }

    wait_not_updating(item);
    if (job->set) job_set(job, item);
    else          job_get(job, item);
    com_release(item);
}

typedef struct {
    HANDLE      ready;
    HANDLE      done;
    SettingJob *job;
} Worker;

static DWORD WINAPI worker_loop(LPVOID arg) {
    Worker *w = (Worker *)arg;
    RoInitialize(RO_INIT_SINGLETHREADED);
    for (;;) {
        DWORD woke = MsgWaitForMultipleObjects(1, &w->ready, FALSE, INFINITE, QS_ALLINPUT);
        if (woke == WAIT_OBJECT_0 + 1) {
            pump_messages();
            continue;
        }
        if (woke != WAIT_OBJECT_0) continue;
        job_run(w->job);
        SetEvent(w->done);
    }
    return 0;
}

static Worker *worker_start(void) {
    Worker *w = (Worker *)calloc(1, sizeof *w);
    if (!w) return NULL;
    w->ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    w->done  = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE thread = (w->ready && w->done)
        ? CreateThread(NULL, 0, worker_loop, w, 0, NULL) : NULL;
    if (!thread) return NULL;
    CloseHandle(thread);
    return w;
}

static Worker *s_worker;

typedef enum {
    JOB_OK,
    JOB_FAILED,
    JOB_DISABLED,
} JobOutcome;

static JobOutcome run_job(SettingJob *job, bool last_chance) {
    memset(&job->report, 0, sizeof job->report);

    if (!s_worker) s_worker = worker_start();
    if (!s_worker) {
        report_fail(&job->report, job->name, L"could not start a worker (error %lu)", GetLastError());
        print_wide(job->report.text);
        return JOB_FAILED;
    }

    s_worker->job = job;
    SetEvent(s_worker->ready);
    if (WaitForSingleObject(s_worker->done, SETTING_TIMEOUT_MS) != WAIT_OBJECT_0) {
        s_worker = NULL;
        wchar_t line[256];
        _snwprintf(line, ARRAYSIZE(line) - 1, L"%ls: FAILED — Windows did not answer within %d seconds",
                   job->name, SETTING_TIMEOUT_MS / 1000);
        line[ARRAYSIZE(line) - 1] = L'\0';
        print_wide(line);
        return JOB_FAILED;
    }

    if (job->report.disabled && !last_chance) return JOB_DISABLED;
    print_wide(job->report.text);
    return job->report.failed ? JOB_FAILED : JOB_OK;
}

static SettingJob *job_new(const SettingField *f, bool set) {
    SettingJob *job = (SettingJob *)calloc(1, sizeof *job);
    if (!job) return NULL;
    job->field = f;
    job->set   = set;
    _snwprintf(job->name, ARRAYSIZE(job->name) - 1, L"%hs", f->path);
    return job;
}

static void to_utf8(const wchar_t *w, char *out, size_t cap) {
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) <= 0) out[0] = '\0';
}

static const SettingField *field_named(const wchar_t *name) {
    char path[128];
    to_utf8(name, path, sizeof path);
    const SettingField *f = settings_catalog_find(path);
    if (!f) {
        wchar_t line[256];
        _snwprintf(line, ARRAYSIZE(line) - 1, L"%ls: FAILED — no such setting "
                   L"(mshell.exe --settings list shows them)", name);
        line[ARRAYSIZE(line) - 1] = L'\0';
        print_wide(line);
    }
    return f;
}

static int settings_list(const wchar_t *pattern) {
    wchar_t wild[128] = L"";
    if (pattern && *pattern) {
        _snwprintf(wild, ARRAYSIZE(wild) - 1, wcspbrk(pattern, L"*?") ? L"%ls" : L"*%ls*", pattern);
        wild[ARRAYSIZE(wild) - 1] = L'\0';
    }

    size_t cap = 256 + (size_t)settings_catalog_count() * 200;
    wchar_t *text = (wchar_t *)calloc(cap, sizeof(wchar_t));
    if (!text) return 1;

    size_t used = 0;
    int shown = 0;
    for (int k = 0; k < settings_catalog_count(); k++) {
        const SettingField *f = settings_catalog_at(k);
        wchar_t path[96];
        _snwprintf(path, ARRAYSIZE(path) - 1, L"%hs", f->path);
        path[ARRAYSIZE(path) - 1] = L'\0';
        if (wild[0] && !wildcard_match(wild, path)) continue;

        char kind[128];
        settings_field_describe(f, kind, sizeof kind);
        int n = _snwprintf(text + used, cap - used - 1, L"%ls%-32ls %-18hs %hs",
                           used ? L"\r\n" : L"", path, kind, f->doc);
        if (n < 0) break;
        used += (size_t)n;
        shown++;
    }

    if (shown == 0) _snwprintf(text, cap - 1, L"no settings match '%ls'", pattern);
    text[cap - 1] = L'\0';
    print_wide(text);
    free(text);
    return shown == 0 && wild[0] ? 1 : 0;
}

static int settings_get(int argc, wchar_t **argv) {
    bool ok = true;
    int  total = argc > 0 ? argc : settings_catalog_count();
    for (int k = 0; k < total; k++) {
        const SettingField *f = argc > 0 ? field_named(argv[k]) : settings_catalog_at(k);
        if (!f) { ok = false; continue; }
        SettingJob *job = job_new(f, false);
        if (!job || run_job(job, true) != JOB_OK) ok = false;
    }
    return ok ? 0 : 1;
}

static int settings_set(int argc, wchar_t **argv) {
    if (argc == 0 || argc % 2 != 0) return -1;

    bool          ok      = true;
    SettingJob  **waiting = (SettingJob **)calloc((size_t)argc, sizeof *waiting);
    int           waits   = 0;
    if (!waiting) return 1;

    for (int k = 0; k + 1 < argc; k += 2) {
        const SettingField *f = field_named(argv[k]);
        if (!f) { ok = false; continue; }

        SettingJob *job = job_new(f, true);
        if (!job) { ok = false; continue; }

        char text[SETTING_FIELD_TEXT_MAX], err[256];
        to_utf8(argv[k + 1], text, sizeof text);
        if (!settings_field_engine_text(f, text, job->want, ARRAYSIZE(job->want), err, sizeof err)) {
            wchar_t line[400];
            _snwprintf(line, ARRAYSIZE(line) - 1, L"%ls: FAILED — %hs", job->name, err);
            line[ARRAYSIZE(line) - 1] = L'\0';
            print_wide(line);
            ok = false;
            continue;
        }

        JobOutcome outcome = run_job(job, false);
        if (outcome == JOB_DISABLED) waiting[waits++] = job;
        else if (outcome == JOB_FAILED) ok = false;
    }

    for (int k = 0; k < waits; k++)
        if (run_job(waiting[k], true) != JOB_OK) ok = false;

    return ok ? 0 : 1;
}

static void settings_usage(void) {
    print_wide(L"usage: mshell --settings list [pattern]\r\n"
               L"       mshell --settings get [name...]\r\n"
               L"       mshell --settings set <name> <value> [<name> <value>...]\r\n"
               L"Names are the config's: mouse.scroll_lines, theme.mode, keyboard.repeat_rate.\r\n"
               L"Changes are saved to your profile, as the Settings app saves them.");
}

int settings_cli(int argc, wchar_t **argv) {
    if (argc < 1 || !wcscmp(argv[0], L"list"))
        return settings_list(argc >= 2 ? argv[1] : NULL);
    if (!wcscmp(argv[0], L"get"))
        return settings_get(argc - 1, argv + 1);
    if (!wcscmp(argv[0], L"set")) {
        int code = settings_set(argc - 1, argv + 1);
        if (code >= 0) return code;
    }
    settings_usage();
    return 1;
}
