/*
 * test_match.c — wildcard_match().
 *
 * This is the function that decides which rule a window gets, for both window
 * rules and desktop rules. Its failure mode is silence: a pattern that stops
 * matching does not crash anything, it just means a game is no longer
 * borderless or a file picker is no longer floated, with nothing in the log.
 * Hence the table.
 */

#include "tests.h"
#include "../src/match.h"

#define YES(pat, str) CHECK(wildcard_match(L##pat, L##str),  \
                            "expected '%ls' to match '%ls'", L##pat, L##str)
#define NO(pat, str)  CHECK(!wildcard_match(L##pat, L##str), \
                            "expected '%ls' NOT to match '%ls'", L##pat, L##str)

int main(void) {
    /* --- exact matching: a pattern with no wildcard --- */
    YES("firefox.exe", "firefox.exe");
    NO ("firefox.exe", "firefox.ex");
    NO ("firefox.ex",  "firefox.exe");
    NO ("firefox.exe", "");

    /* --- case insensitivity --- */
    YES("FireFox.EXE", "firefox.exe");
    YES("firefox.exe", "FIREFOX.EXE");
    YES("UnityWndClass", "unitywndclass");

    /* --- '?' is exactly one character, never zero --- */
    YES("a?c", "abc");
    NO ("a?c", "ac");
    NO ("a?c", "abbc");
    YES("???", "xyz");
    NO ("???", "xy");

    /* --- '*' is any run, including empty --- */
    YES("*", "");
    YES("*", "anything at all");
    YES("a*", "a");
    YES("a*", "abcdef");
    YES("*z", "z");
    YES("*z", "abcz");
    YES("a*z", "az");
    YES("a*z", "abcz");
    NO ("a*z", "abc");
    NO ("*z", "za");

    /* --- backtracking: the greedy star has to give characters back --- */
    YES("*ab", "aab");
    YES("*ab", "aaab");
    YES("a*b*c", "abc");
    YES("a*b*c", "axxbyyc");
    NO ("a*b*c", "axxbyy");
    YES("**a", "a");          /* consecutive stars collapse */
    YES("*a*a*a", "aaa");
    NO ("*a*a*a*a", "aaa");

    /* --- '/' and '\' fold together, so a path can be written either way --- */
    YES("C:/Games/*",  "C:\\Games\\doom.exe");
    YES("C:\\Games\\*", "C:/Games/doom.exe");
    YES("*/steamapps/common/*", "D:\\SteamLibrary\\steamapps\\common\\Portal\\portal.exe");

    /* --- the real patterns from the shipped config --- */
    YES("*\\steamapps\\common\\*", "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Half-Life\\hl.exe");
    YES("*\\steamapps\\common\\*", "E:\\SteamLibrary\\steamapps\\common\\Elden Ring\\eldenring.exe");
    NO ("*\\steamapps\\common\\*", "C:\\Program Files\\Mozilla Firefox\\firefox.exe");
    YES("*\\Riot Games\\VALORANT\\*",
        "C:\\Riot Games\\VALORANT\\live\\ShooterGame\\Binaries\\Win64\\VALORANT-Win64-Shipping.exe");
    NO ("*\\Riot Games\\VALORANT\\*", "C:\\Riot Games\\Riot Client\\RiotClientServices.exe");
    YES("*\\Riot Games\\Riot Client\\*", "C:\\Riot Games\\Riot Client\\RiotClientServices.exe");
    YES("*launcher*", "GameLauncher.exe");
    YES("*launcher*", "launcher.exe");
    YES("*crash*",    "CrashReporter.exe");
    NO ("*launcher*", "eldenring.exe");

    /* --- desktop-name patterns --- */
    YES("game-*", "game-1");
    YES("game-*", "game-");
    NO ("game-*", "game");
    YES("*", "web");

    /* --- empty pattern matches only the empty string. Callers treat "no
     *     pattern given" as "matches anything" BEFORE calling in, so this must
     *     not quietly become a wildcard. --- */
    YES("", "");
    NO ("", "x");

    return tests_report("match");
}
