#pragma once

/*
 * pipe_sd.h — the security descriptor mshell's named pipes are created with.
 *
 * Shared by mshell.exe (ipc.c, the --msg channel) and mshelld.exe (the
 * privileged helper). Like log.c, this unit deliberately depends on nothing but
 * the Win32 API — no mshell.h, no MShell global — precisely so it can link into
 * the helper, which has no access to the shell's types or state.
 *
 * WHY IT IS SHARED. Both pipes want the same answer: only the user running this
 * session may open it. ipc.c already computed that correctly from its own
 * process token; the helper hardcoded the SDDL alias "IU" instead, which means
 * any INTERACTIVE user — and since the pipe name is per-session and entirely
 * predictable, a second user signed in via fast user switching could open the
 * first user's helper pipe and move, hide or close their windows. One
 * implementation, in one place, is how the two stop being allowed to disagree.
 *
 * The helper runs elevated, which does not change the answer: elevation gives a
 * process a different integrity level and different groups, not a different
 * user. The logon task INSTALL.md registers (`schtasks /rl highest`, no `/ru`)
 * runs as the interactive user, so its token's user SID is the same one the
 * unelevated shell has. A helper registered to run as SYSTEM would grant SYSTEM
 * and the shell could not connect — which is also why that is unsupported, and
 * why the SID is logged.
 */

#include <windows.h>

/*
 * Build a DACL granting GENERIC_ALL to the calling process's own user and to
 * SYSTEM, and nothing to anybody else. Free the result with LocalFree().
 *
 * Returns NULL on failure. Callers must then REFUSE to create the pipe rather
 * than falling back to a NULL descriptor, which is reachable by every local
 * account.
 *
 * `sid_out`, when non-NULL, receives the granted user's SID in string form, so
 * a caller can log which identity it just handed the pipe to. That is the whole
 * diagnosis when a misconfigured helper task cannot be connected to.
 */
PSECURITY_DESCRIPTOR pipe_sd_for_current_user(wchar_t *sid_out, size_t sid_cap);
