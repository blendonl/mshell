#pragma once

/*
 * update_parse.h — reading GitHub's release JSON, and comparing versions.
 *
 * Split out of update.c on the same terms as match.c and layout_math.c: this
 * file depends on nothing but the C library, so `make test` can compile and
 * run it natively instead of it only ever executing on a Windows machine
 * during an upgrade — which is the worst possible place to discover a parsing
 * bug, because the thing it breaks is the mechanism you would fix it with.
 *
 * It is hand-written scanning rather than a JSON parser, which is the right
 * trade for one known shape from one known endpoint but does have to be
 * careful in one specific way. Finding where one asset ends and the next
 * begins means finding a matching brace, and braces also occur INSIDE strings
 * here: the uploader's template URLs ("{owner}{repo}") contain them, and an
 * asset's label is free text that can contain a lone one. The templates happen
 * to be balanced, so counting braces survives them by luck — a stray "{" in a
 * label is not survivable, and runs the scan past every asset that follows.
 * So the scanning steps over string literals rather than counting through
 * them.
 */

#include <stdbool.h>
#include <stddef.h>

/* Compare dotted versions numerically: "0.9.0" < "0.10.0", which strcmp gets
 * backwards. Returns >0 when `a` is newer, <0 when `b` is, 0 when they are
 * equal — or when they differ only past a non-numeric suffix, which stops the
 * comparison rather than being guessed at: a release-candidate scheme mshell
 * does not use must not silently read as newer.
 *
 * A leading "v" is accepted on either side, so a tag can be passed as-is. */
int update_version_cmp(const char *a, const char *b);

/* Copy the string value of `key` from the JSON region [start,end).
 * The key is matched with its quotes, so "url" cannot match inside
 * "browser_download_url". False when the key is absent or its value is empty.
 */
bool update_json_str(const char *start, const char *end, const char *key,
                     char *out, size_t cap);

/* Find the release asset whose name ends in `suffix` (case-insensitively) and
 * copy out its name, download URL and digest.
 *
 * The three come from the SAME asset object: a release ships both a .zip and
 * an .msi, and pairing one asset's URL with another's hash would fail the
 * integrity check in the most confusing way available. `digest` is set to an
 * empty string when the release predates GitHub publishing the field — that
 * costs the caller its integrity check, not the update.
 *
 * False when there is no assets array or nothing in it matches.
 */
bool update_find_asset(const char *json, const char *suffix,
                       char *name,   size_t name_cap,
                       char *url,    size_t url_cap,
                       char *digest, size_t digest_cap);
