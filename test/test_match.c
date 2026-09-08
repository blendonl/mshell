#include "tests.h"
#include "../src/match.h"

#define YES(pat, str) CHECK(wildcard_match(L##pat, L##str),  \
                            "expected '%ls' to match '%ls'", L##pat, L##str)
#define NO(pat, str)  CHECK(!wildcard_match(L##pat, L##str), \
                            "expected '%ls' NOT to match '%ls'", L##pat, L##str)

int main(void) {
    YES("firefox.exe", "firefox.exe");
    NO ("firefox.exe", "firefox.ex");
    NO ("firefox.ex",  "firefox.exe");
    NO ("firefox.exe", "");

    YES("FireFox.EXE", "firefox.exe");
    YES("firefox.exe", "FIREFOX.EXE");
    YES("UnityWndClass", "unitywndclass");

    YES("a?c", "abc");
    NO ("a?c", "ac");
    NO ("a?c", "abbc");
    YES("???", "xyz");
    NO ("???", "xy");

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

    YES("*ab", "aab");
    YES("*ab", "aaab");
    YES("a*b*c", "abc");
    YES("a*b*c", "axxbyyc");
    NO ("a*b*c", "axxbyy");
    YES("**a", "a");
    YES("*a*a*a", "aaa");
    NO ("*a*a*a*a", "aaa");

    YES("C:/Games/*",  "C:\\Games\\doom.exe");
    YES("C:\\Games\\*", "C:/Games/doom.exe");
    YES("*/steamapps/common/*", "D:\\SteamLibrary\\steamapps\\common\\Portal\\portal.exe");

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

    YES("game-*", "game-1");
    YES("game-*", "game-");
    NO ("game-*", "game");
    YES("*", "web");

    YES("", "");
    NO ("", "x");

    return tests_report("match");
}
