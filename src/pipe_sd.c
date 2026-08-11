/*
 * pipe_sd.c — see pipe_sd.h for why this is its own unit and why it is shared.
 */

#include "pipe_sd.h"

#include <sddl.h>       /* ConvertSidToStringSidW, ConvertStringSecurity...  */
#include <stdio.h>      /* _snwprintf                                        */
#include <stdlib.h>     /* malloc/free                                       */

PSECURITY_DESCRIPTOR pipe_sd_for_current_user(wchar_t *sid_out, size_t sid_cap) {
    if (sid_out && sid_cap) sid_out[0] = L'\0';

    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return NULL;

    DWORD len = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &len);
    if (!len) { CloseHandle(token); return NULL; }

    TOKEN_USER *tu = (TOKEN_USER *)malloc(len);
    if (!tu) { CloseHandle(token); return NULL; }

    PSECURITY_DESCRIPTOR sd = NULL;
    if (GetTokenInformation(token, TokenUser, tu, len, &len)) {
        LPWSTR sid_str = NULL;
        if (ConvertSidToStringSidW(tu->User.Sid, &sid_str)) {
            /* GA = generic all, to the owning user and to SYSTEM. No other ACE,
             * and no inheritance: nothing else may open this pipe. In
             * particular NOT the "IU" alias, which is every interactively
             * logged-on user and therefore every other session on the machine.
             */
            wchar_t sddl[512];
            _snwprintf(sddl, 512, L"D:(A;;GA;;;%ls)(A;;GA;;;SY)", sid_str);
            sddl[511] = L'\0';

            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    sddl, SDDL_REVISION_1, &sd, NULL))
                sd = NULL;

            if (sd && sid_out && sid_cap) {
                wcsncpy(sid_out, sid_str, sid_cap - 1);
                sid_out[sid_cap - 1] = L'\0';
            }

            LocalFree(sid_str);
        }
    }

    free(tu);
    CloseHandle(token);
    return sd;
}
