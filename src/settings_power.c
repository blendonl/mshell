#include "mshell.h"

#include <powrprof.h>
#include <powersetting.h>

static DWORD power_target(const SettingField *f, GUID *subgroup, GUID *setting) {
    if (FAILED(IIDFromString(f->power_subgroup, subgroup)) ||
        FAILED(IIDFromString(f->power_setting, setting)))
        return ERROR_INVALID_PARAMETER;
    return ERROR_SUCCESS;
}

DWORD settings_power_read(const SettingField *f, DWORD *value) {
    GUID  subgroup, setting;
    DWORD rc = power_target(f, &subgroup, &setting);
    if (rc != ERROR_SUCCESS) return rc;

    GUID *scheme = NULL;
    rc = PowerGetActiveScheme(NULL, &scheme);
    if (rc != ERROR_SUCCESS) return rc;

    rc = f->power_battery
        ? PowerReadDCValueIndex(NULL, scheme, &subgroup, &setting, value)
        : PowerReadACValueIndex(NULL, scheme, &subgroup, &setting, value);
    LocalFree(scheme);
    return rc;
}

DWORD settings_power_write(const SettingField *f, DWORD value) {
    GUID  subgroup, setting;
    DWORD rc = power_target(f, &subgroup, &setting);
    if (rc != ERROR_SUCCESS) return rc;

    GUID *scheme = NULL;
    rc = PowerGetActiveScheme(NULL, &scheme);
    if (rc != ERROR_SUCCESS) return rc;

    rc = f->power_battery
        ? PowerWriteDCValueIndex(NULL, scheme, &subgroup, &setting, value)
        : PowerWriteACValueIndex(NULL, scheme, &subgroup, &setting, value);
    if (rc == ERROR_SUCCESS) rc = PowerSetActiveScheme(NULL, scheme);
    LocalFree(scheme);
    return rc;
}
