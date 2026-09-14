#include "../src/settings_parse.h"
#include "tests.h"

static bool parses(SettingValueKind kind, const wchar_t *text, SettingValue *out) {
    const char *why = NULL;
    return settings_parse_value(kind, text, out, &why);
}

static void test_types(void) {
    CHECK(wcscmp(settings_type_name(SETTING_LIST), L"List") == 0, "name of List");
    CHECK(wcscmp(settings_type_name(SETTING_UNKNOWN), L"Unknown") == 0, "name of unknown");
    CHECK(settings_type_writable(SETTING_BOOLEAN), "Boolean writable");
    CHECK(settings_type_writable(SETTING_LIST), "List writable");
    CHECK(!settings_type_writable(SETTING_ACTION), "Action is not a value");
    CHECK(!settings_type_writable(SETTING_DISPLAY_STRING), "DisplayString is read-only");
}

static void test_bool(void) {
    SettingValue v;
    CHECK(parses(SV_BOOL, L"true", &v) && v.b, "true");
    CHECK(parses(SV_BOOL, L"OFF", &v) && !v.b, "OFF");
    CHECK(parses(SV_BOOL, L" yes ", &v) && v.b, "trimmed yes");
    CHECK(parses(SV_BOOL, L"0", &v) && !v.b, "0");
    CHECK(!parses(SV_BOOL, L"2", &v), "2 is not a boolean");
    CHECK(!parses(SV_BOOL, L"", &v), "empty");

    const char *why = NULL;
    CHECK(!settings_parse_value(SV_BOOL, L"maybe", &v, &why) && why != NULL, "reason given");
}

static void test_numbers(void) {
    SettingValue v;
    CHECK(parses(SV_UINT32, L"20", &v) && v.u == 20, "uint32 20");
    CHECK(parses(SV_UINT32, L"4294967295", &v) && v.u == 4294967295ULL, "uint32 max");
    CHECK(!parses(SV_UINT32, L"4294967296", &v), "uint32 overflow");
    CHECK(!parses(SV_UINT32, L"-1", &v), "uint32 negative");
    CHECK(!parses(SV_UINT32, L"10px", &v), "trailing junk");
    CHECK(!parses(SV_UINT32, L"3.5", &v), "fraction for an integer");
    CHECK(parses(SV_INT32, L"-5", &v) && v.i == -5, "int32 negative");
    CHECK(!parses(SV_INT32, L"2147483648", &v), "int32 overflow");
    CHECK(parses(SV_INT64, L"-9000000000", &v) && v.i == -9000000000LL, "int64");
    CHECK(parses(SV_DOUBLE, L"0.25", &v) && v.d == 0.25, "double");
    CHECK(!parses(SV_DOUBLE, L"nan", &v), "nan rejected");
    CHECK(!parses(SV_DOUBLE, L"fast", &v), "not a number");
}

static void test_string(void) {
    SettingValue v;
    CHECK(parses(SV_STRING, L"  Dark ", &v) && wcscmp(v.s, L"Dark") == 0, "trimmed string");

    wchar_t huge[SETTING_TEXT_MAX + 8];
    for (size_t k = 0; k < SETTING_TEXT_MAX + 4; k++) huge[k] = L'x';
    huge[SETTING_TEXT_MAX + 4] = L'\0';
    CHECK(!parses(SV_STRING, huge, &v), "too long");
    CHECK(!parses(SV_NONE, L"1", &v), "no kind to parse into");
}

static void test_equal_and_range(void) {
    SettingValue a, b, lo, hi;
    parses(SV_UINT32, L"10", &a);
    parses(SV_UINT32, L"10", &b);
    CHECK(settings_value_equal(&a, &b), "same uint");
    parses(SV_UINT32, L"11", &b);
    CHECK(!settings_value_equal(&a, &b), "different uint");
    parses(SV_INT32, L"10", &b);
    CHECK(!settings_value_equal(&a, &b), "different kinds never equal");

    parses(SV_STRING, L"Dark", &a);
    parses(SV_STRING, L"dark", &b);
    CHECK(!settings_value_equal(&a, &b), "strings compare exactly");

    parses(SV_UINT32, L"1", &lo);
    parses(SV_UINT32, L"20", &hi);
    parses(SV_UINT32, L"20", &a);
    CHECK(settings_value_in_range(&a, &lo, &hi), "upper bound inclusive");
    parses(SV_UINT32, L"21", &a);
    CHECK(!settings_value_in_range(&a, &lo, &hi), "above max");
    parses(SV_UINT32, L"0", &a);
    CHECK(!settings_value_in_range(&a, &lo, &hi), "below min");
    CHECK(settings_value_in_range(&a, NULL, NULL), "no bounds");
    parses(SV_STRING, L"x", &a);
    CHECK(settings_value_in_range(&a, &lo, &hi), "strings are not range-checked");
}

static void test_format(void) {
    SettingValue v;
    wchar_t out[64];

    parses(SV_BOOL, L"on", &v);
    settings_format_value(&v, out, 64);
    CHECK(wcscmp(out, L"true") == 0, "bool formats as true");

    parses(SV_INT32, L"-7", &v);
    settings_format_value(&v, out, 64);
    CHECK(wcscmp(out, L"-7") == 0, "int formats");

    parses(SV_UINT64, L"18446744073709551615", &v);
    settings_format_value(&v, out, 64);
    CHECK(wcscmp(out, L"18446744073709551615") == 0, "uint64 max formats");

    parses(SV_STRING, L"Light", &v);
    settings_format_value(&v, out, 4);
    CHECK(wcslen(out) < 4, "truncates to the buffer");
}

static void test_args(void) {
    wchar_t buf[128];
    size_t n = settings_append_arg(buf, 128, 0, L"--settings");
    CHECK(n == 10 && wcscmp(buf, L"--settings") == 0, "bare arg");

    n = settings_append_arg(buf, 128, n, L"0.5 seconds");
    CHECK(wcscmp(buf, L"--settings \"0.5 seconds\"") == 0, "spaces are quoted");

    n = settings_append_arg(buf, 128, 0, L"");
    CHECK(n == 2 && wcscmp(buf, L"\"\"") == 0, "empty arg is quoted");

    n = settings_append_arg(buf, 128, 0, L"say \"hi\"");
    CHECK(wcscmp(buf, L"\"say \\\"hi\\\"\"") == 0, "embedded quotes escaped");

    n = settings_append_arg(buf, 128, 0, L"C:\\dir with space\\");
    CHECK(wcscmp(buf, L"\"C:\\dir with space\\\\\"") == 0, "trailing backslash doubled inside quotes");

    n = settings_append_arg(buf, 128, 0, L"C:\\plain\\");
    CHECK(wcscmp(buf, L"C:\\plain\\") == 0, "unquoted backslashes untouched");

    n = settings_append_arg(buf, 128, 0, L"a\\\"b");
    CHECK(wcscmp(buf, L"\"a\\\\\\\"b\"") == 0, "backslashes before a quote doubled");

    wchar_t tiny[8];
    CHECK(settings_append_arg(tiny, 8, 0, L"abcdefgh") == 0, "overflow reports 0");
    CHECK(settings_append_arg(tiny, 8, 0, L"abcdefg") == 7 && tiny[7] == L'\0',
          "exactly fills the buffer with its terminator");
    CHECK(settings_append_arg(tiny, 8, 5, L"xy") == 0, "separator pushes it over");
}

int main(void) {
    test_types();
    test_bool();
    test_numbers();
    test_string();
    test_equal_and_range();
    test_format();
    test_args();
    return tests_report("settings_parse");
}
