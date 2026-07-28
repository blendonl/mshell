/*
 * test_update_parse.c — the release-JSON reading behind the `update` action.
 *
 * The fixtures below are shaped like a real /releases/latest response, which
 * matters more than their size: a release ships BOTH a .zip and an .msi, so
 * the URL and the hash have to be read from the same asset, and asset objects
 * contain braces inside strings, so their boundaries cannot be found by
 * counting. The real uploader templates ("{owner}{repo}") are reproduced here
 * but are balanced and prove nothing on their own — ASSET_STRAY_BRACE below is
 * the fixture that actually holds that behaviour in place.
 */
#include "../src/update_parse.h"
#include "tests.h"

/* One asset object. `n` is the file name, `d` the digest, and the uploader is
 * included verbatim because its braces are the hazard. */
#define ASSET(n, d)                                                           \
    "{\"url\":\"https://api.github.com/repos/blendonl/mshell/releases/"       \
    "assets/1\",\"id\":1,\"node_id\":\"RA_k\",\"name\":\"" n "\","            \
    "\"label\":\"\",\"uploader\":{\"login\":\"github-actions[bot]\","         \
    "\"starred_url\":\"https://api.github.com/users/x/starred{/owner}{/repo}\","\
    "\"following_url\":\"https://api.github.com/users/x/following{/other_user}\","\
    "\"type\":\"Bot\"},\"content_type\":\"application/zip\","                 \
    "\"state\":\"uploaded\",\"size\":415650,\"digest\":\"" d "\","            \
    "\"download_count\":0,\"browser_download_url\":"                          \
    "\"https://github.com/blendonl/mshell/releases/download/v0.13.4/" n "\"}"

#define ZIP_SHA "sha256:191f2492a9a7c9c630e8cb4c2bc3b71bd6130bb566999388e423b07739031f14"
#define MSI_SHA "sha256:145bdeb0219667288f22cc26eace398b008b2516e5fa1c2d56c3fed5921d50b8"

/* zip first, then msi — the order GitHub actually returned. */
static const char *RELEASE =
    "{\"url\":\"https://api.github.com/repos/blendonl/mshell/releases/361106558\","
    "\"upload_url\":\"https://uploads.github.com/repos/blendonl/mshell/releases/"
    "361106558/assets{?name,label}\","
    "\"html_url\":\"https://github.com/blendonl/mshell/releases/tag/v0.13.4\","
    "\"tag_name\":\"v0.13.4\",\"name\":\"mshell 0.13.4\",\"draft\":false,"
    "\"prerelease\":false,\"assets\":["
        ASSET("mshell-0.13.4-win64.zip", ZIP_SHA) ","
        ASSET("mshell-0.13.4.msi",       MSI_SHA)
    "],\"body\":\"### Added\\n- things\"}";

/* msi first: which asset wins must not depend on the order they arrive in. */
static const char *RELEASE_MSI_FIRST =
    "{\"tag_name\":\"v0.13.4\",\"assets\":["
        ASSET("mshell-0.13.4.msi",       MSI_SHA) ","
        ASSET("mshell-0.13.4-win64.zip", ZIP_SHA)
    "]}";

/* An asset carrying an UNBALANCED brace in a free-text field. `label` is
 * whoever-uploaded-it's prose, so this is the case that makes stepping over
 * string literals load-bearing rather than merely tidy: the uploader templates
 * above happen to be balanced, so brace counting survives them by luck, but
 * one stray "{" here runs a naive scan past the end of the asset it is in and
 * swallows every asset after it — including the zip we came for. */
#define ASSET_STRAY_BRACE                                                     \
    "{\"name\":\"mshell-0.13.4.msi\",\"label\":\"release {candidate\","       \
    "\"uploader\":{\"login\":\"x\"},\"digest\":\"" MSI_SHA "\","              \
    "\"browser_download_url\":\"https://example.invalid/mshell.msi\"}"

static const char *RELEASE_STRAY_BRACE =
    "{\"tag_name\":\"v0.13.4\",\"assets\":["
        ASSET_STRAY_BRACE ","
        ASSET("mshell-0.13.4-win64.zip", ZIP_SHA)
    "]}";

/* The mirror image: a stray closing brace, which closes the object early and
 * makes the scan read the NEXT asset's fields as if they belonged to this one. */
#define ASSET_STRAY_CLOSE                                                     \
    "{\"name\":\"mshell-0.13.4.msi\",\"label\":\"release }candidate\","       \
    "\"uploader\":{\"login\":\"x\"},\"digest\":\"" MSI_SHA "\","              \
    "\"browser_download_url\":\"https://example.invalid/mshell.msi\"}"

static const char *RELEASE_STRAY_CLOSE =
    "{\"tag_name\":\"v0.13.4\",\"assets\":["
        ASSET_STRAY_CLOSE ","
        ASSET("mshell-0.13.4-win64.zip", ZIP_SHA)
    "]}";

/* A release from before GitHub published per-asset digests. */
static const char *RELEASE_NO_DIGEST =
    "{\"tag_name\":\"v0.12.0\",\"assets\":[{"
    "\"name\":\"mshell-0.12.0-win64.zip\",\"uploader\":{\"login\":\"x\"},"
    "\"browser_download_url\":\"https://github.com/blendonl/mshell/releases/"
    "download/v0.12.0/mshell-0.12.0-win64.zip\"}]}";

static const char *RELEASE_NO_ZIP =
    "{\"tag_name\":\"v0.13.4\",\"assets\":["
        ASSET("mshell-0.13.4.msi", MSI_SHA)
    "]}";

static const char *end_of(const char *s) { return s + strlen(s); }

int main(void) {
    /* --- version comparison ------------------------------------------- */

    /* The case a strcmp gets backwards, and the reason this is not a strcmp. */
    CHECK(update_version_cmp("0.10.0", "0.9.0") > 0, "0.10.0 is newer than 0.9.0");
    CHECK(update_version_cmp("0.9.0", "0.10.0") < 0, "0.9.0 is older than 0.10.0");

    CHECK(update_version_cmp("0.13.4", "0.13.4") == 0, "equal versions");
    CHECK(update_version_cmp("0.13.4", "0.13.3") > 0, "patch bump is newer");
    CHECK(update_version_cmp("1.0.0",  "0.99.99") > 0, "major beats minor");
    CHECK(update_version_cmp("0.14.0", "0.13.4") > 0, "minor bump is newer");

    /* A tag may be passed with its "v" still attached. */
    CHECK(update_version_cmp("v0.13.4", "0.13.3") > 0, "leading v on the left");
    CHECK(update_version_cmp("0.13.4", "v0.13.4") == 0, "leading v on the right");

    /* Not a scheme mshell uses; it must not read as newer either way. */
    CHECK(update_version_cmp("0.13.4-rc1", "0.13.4") == 0, "suffix stops the compare");

    /* The one that actually gates the action: a release equal to or older than
     * what is running must never come out positive. */
    CHECK(update_version_cmp("0.13.4", "0.13.4") <= 0, "same version does not update");
    CHECK(update_version_cmp("0.13.3", "0.13.4") <= 0, "older release does not update");

    /* --- field extraction --------------------------------------------- */

    char buf[256];

    CHECK(update_json_str(RELEASE, end_of(RELEASE), "tag_name", buf, sizeof(buf)) &&
          strcmp(buf, "v0.13.4") == 0, "tag_name is v0.13.4, got '%s'", buf);

    /* "url" must not match inside "browser_download_url" or "html_url" — the
     * whole reason the key is matched with both its quotes. */
    CHECK(update_json_str(RELEASE, end_of(RELEASE), "url", buf, sizeof(buf)) &&
          strcmp(buf, "https://api.github.com/repos/blendonl/mshell/releases/361106558") == 0,
          "url is the release url, got '%s'", buf);

    CHECK(!update_json_str(RELEASE, end_of(RELEASE), "nonexistent", buf, sizeof(buf)),
          "absent key returns false");

    /* draft is a bool, not a string: reading it as one must fail rather than
     * return something confident and wrong. */
    CHECK(!update_json_str(RELEASE, end_of(RELEASE), "draft", buf, sizeof(buf)),
          "non-string value returns false");

    /* --- asset selection ---------------------------------------------- */

    char name[256], url[512], digest[128];

    CHECK(update_find_asset(RELEASE, "-win64.zip", name, sizeof(name),
                            url, sizeof(url), digest, sizeof(digest)),
          "the zip asset is found");
    CHECK(strcmp(name, "mshell-0.13.4-win64.zip") == 0,
          "picked the zip, got '%s'", name);
    CHECK(strcmp(url, "https://github.com/blendonl/mshell/releases/download/"
                      "v0.13.4/mshell-0.13.4-win64.zip") == 0,
          "zip download url, got '%s'", url);

    /* The pairing that matters: the digest must be the ZIP's, never the MSI's.
     * Getting this wrong fails the integrity check on a perfectly good file. */
    CHECK(strcmp(digest, ZIP_SHA) == 0, "digest belongs to the zip, got '%s'", digest);
    CHECK(strcmp(digest, MSI_SHA) != 0, "digest is not the msi's");

    /* Same answer regardless of the order the assets arrive in. */
    CHECK(update_find_asset(RELEASE_MSI_FIRST, "-win64.zip", name, sizeof(name),
                            url, sizeof(url), digest, sizeof(digest)),
          "the zip is found with the msi listed first");
    CHECK(strcmp(name, "mshell-0.13.4-win64.zip") == 0,
          "still picked the zip, got '%s'", name);
    CHECK(strcmp(digest, ZIP_SHA) == 0,
          "still the zip's digest, got '%s'", digest);

    /* A stray brace inside a string must not extend the asset it appears in
     * over the ones that follow. Both directions: an unmatched "{" would run
     * past the zip and find nothing, an unmatched "}" would end the object
     * early and read the zip's fields as the msi's. */
    CHECK(update_find_asset(RELEASE_STRAY_BRACE, "-win64.zip", name, sizeof(name),
                            url, sizeof(url), digest, sizeof(digest)),
          "an unmatched { in a label does not hide the zip");
    CHECK(strcmp(name, "mshell-0.13.4-win64.zip") == 0,
          "still the zip past a stray {, got '%s'", name);
    CHECK(strcmp(digest, ZIP_SHA) == 0,
          "still the zip's digest past a stray {, got '%s'", digest);

    CHECK(update_find_asset(RELEASE_STRAY_CLOSE, "-win64.zip", name, sizeof(name),
                            url, sizeof(url), digest, sizeof(digest)),
          "an unmatched } in a label does not hide the zip");
    CHECK(strcmp(name, "mshell-0.13.4-win64.zip") == 0,
          "still the zip past a stray }, got '%s'", name);
    CHECK(strcmp(digest, ZIP_SHA) == 0,
          "still the zip's digest past a stray }, got '%s'", digest);

    /* Matching is case-insensitive on the suffix. */
    CHECK(update_find_asset(RELEASE, "-WIN64.ZIP", name, sizeof(name),
                            url, sizeof(url), digest, sizeof(digest)),
          "suffix matching ignores case");

    /* No digest published: found, with the field emptied rather than left
     * holding a previous call's value. */
    strcpy(digest, "stale");
    CHECK(update_find_asset(RELEASE_NO_DIGEST, "-win64.zip", name, sizeof(name),
                            url, sizeof(url), digest, sizeof(digest)),
          "an asset with no digest is still found");
    CHECK(digest[0] == '\0', "missing digest comes back empty, got '%s'", digest);

    /* Nothing matches, and nothing is invented. */
    CHECK(!update_find_asset(RELEASE_NO_ZIP, "-win64.zip", name, sizeof(name),
                             url, sizeof(url), digest, sizeof(digest)),
          "a release with no zip asset finds nothing");
    CHECK(!update_find_asset("{\"tag_name\":\"v1\"}", "-win64.zip",
                             name, sizeof(name), url, sizeof(url),
                             digest, sizeof(digest)),
          "a release with no assets array finds nothing");
    CHECK(!update_find_asset("{\"assets\":[]}", "-win64.zip",
                             name, sizeof(name), url, sizeof(url),
                             digest, sizeof(digest)),
          "an empty assets array finds nothing");

    /* Truncated JSON must terminate rather than run off the end. */
    CHECK(!update_find_asset("{\"assets\":[{\"name\":\"mshell-1-win64.zip\"",
                             "-win64.zip", name, sizeof(name), url, sizeof(url),
                             digest, sizeof(digest)),
          "a truncated response finds nothing");

    return tests_report("update_parse");
}
