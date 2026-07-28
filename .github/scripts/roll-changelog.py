#!/usr/bin/env python3
"""Turn the Unreleased section into a released one, and emit its notes.

CHANGELOG.md is written by hand and is the best documentation this project
has — the entries explain why a change was made, not just that it was. So the
release does not generate prose over the top of it. It promotes what is
already there:

    ## Unreleased          ->    ## Unreleased
    ### Added
    - a thing                    ## 0.13.4 - 2026-07-28
                                 ### Added
                                 - a thing

Because every push to main releases, some releases will arrive with nothing
written under Unreleased. A release with empty notes is worse than a
mechanical one, so in that case only, the commit subjects are used instead.
That fallback is meant to be the ugly path — if it shows up in a release you
cared about, the fix is to write the entry, not to improve the generator.
"""
import argparse
import re
import sys

HEADING = re.compile(r"^## ", re.M)


def split_unreleased(text):
    """Return (before, body, after) around the Unreleased section's body."""
    m = re.search(r"^## Unreleased[^\n]*\n", text, re.M)
    if not m:
        sys.exit("error: no '## Unreleased' heading in CHANGELOG.md")

    body_start = m.end()
    nxt = HEADING.search(text, body_start)
    body_end = nxt.start() if nxt else len(text)
    return text[: m.start()], text[body_start:body_end], text[body_end:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--date", required=True)
    ap.add_argument("--fallback", required=True, help="notes to use if Unreleased is empty")
    ap.add_argument("--notes-out", required=True)
    ap.add_argument("--changelog", default="CHANGELOG.md")
    args = ap.parse_args()

    with open(args.changelog, encoding="utf-8") as f:
        text = f.read()

    before, body, after = split_unreleased(text)

    generated = False
    if not body.strip():
        with open(args.fallback, encoding="utf-8") as f:
            body = "\n" + f.read().strip() + "\n"
        generated = True
        print("note: Unreleased was empty - falling back to commit subjects")

    body = "\n" + body.strip("\n") + "\n"

    # The em dash matches every heading already in the file.
    rolled = (
        before
        + "## Unreleased\n\n"
        + f"## {args.version} — {args.date}\n"
        + body
        + "\n"
        + after
    )

    with open(args.changelog, "w", encoding="utf-8") as f:
        f.write(rolled)

    with open(args.notes_out, "w", encoding="utf-8") as f:
        f.write(body.strip("\n") + "\n")

    print(f"rolled Unreleased into {args.version} ({'generated' if generated else 'hand-written'} notes)")


if __name__ == "__main__":
    main()
