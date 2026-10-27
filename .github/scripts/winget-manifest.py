#!/usr/bin/env python3
import argparse
import hashlib
import pathlib

PACKAGE = "blendonl.mshell"
REPO = "https://github.com/blendonl/mshell"
MANIFEST_VERSION = "1.6.0"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def header(version, kind):
    return [
        f"# yaml-language-server: $schema=https://aka.ms/winget-manifest.{kind}.{MANIFEST_VERSION}.schema.json",
        "",
        f"PackageIdentifier: {PACKAGE}",
        f"PackageVersion: {version}",
    ]


def footer(kind):
    return [f"ManifestType: {kind}", f"ManifestVersion: {MANIFEST_VERSION}", ""]


def version_manifest(version):
    return header(version, "version") + ["DefaultLocale: en-US"] + footer("version")


def installer_manifest(version, msi, product_code):
    lines = header(version, "installer") + [
        "InstallerType: wix",
        "Scope: machine",
        "UpgradeBehavior: install",
        "InstallModes:",
        "  - interactive",
        "  - silent",
        "  - silentWithProgress",
    ]
    if product_code:
        lines.append(f"ProductCode: '{product_code}'")
    lines += [
        "Installers:",
        "  - Architecture: x64",
        f"    InstallerUrl: {REPO}/releases/download/v{version}/{msi.name}",
        f"    InstallerSha256: {sha256(msi)}",
    ]
    return lines + footer("installer")


def locale_manifest(version):
    return header(version, "defaultLocale") + [
        "PackageLocale: en-US",
        "Publisher: blendonl",
        "PublisherUrl: https://github.com/blendonl",
        f"PublisherSupportUrl: {REPO}/issues",
        "PackageName: mshell",
        f"PackageUrl: {REPO}",
        "License: MIT",
        f"LicenseUrl: {REPO}/blob/main/LICENSE",
        "ShortDescription: A keyboard-driven tiling window manager that replaces explorer.exe as the Windows shell.",
        "Moniker: mshell",
        "Tags:",
        "  - tiling-window-manager",
        "  - window-manager",
        "  - shell",
        "  - lua",
        f"ReleaseNotesUrl: {REPO}/releases/tag/v{version}",
    ] + footer("defaultLocale")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--version", required=True)
    p.add_argument("--msi", required=True, type=pathlib.Path)
    p.add_argument("--product-code", default="")
    p.add_argument("--out", required=True, type=pathlib.Path)
    a = p.parse_args()

    out = a.out / "manifests" / PACKAGE[0].lower() / "blendonl" / "mshell" / a.version
    out.mkdir(parents=True, exist_ok=True)

    files = {
        f"{PACKAGE}.yaml": version_manifest(a.version),
        f"{PACKAGE}.installer.yaml": installer_manifest(a.version, a.msi, a.product_code),
        f"{PACKAGE}.locale.en-US.yaml": locale_manifest(a.version),
    }
    for name, lines in files.items():
        (out / name).write_text("\n".join(lines), encoding="utf-8")
        print(out / name)


if __name__ == "__main__":
    main()
