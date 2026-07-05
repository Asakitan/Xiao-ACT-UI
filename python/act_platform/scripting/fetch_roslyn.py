# -*- coding: utf-8 -*-
# Download Roslyn (Microsoft.CodeAnalysis.CSharp) NuGet packages.
#
# Run as::
#
# python -m act_platform.scripting.fetch_roslyn
#
# Downloads the required DLLs into ``scripting/roslyn/`` so that .cs plugin
# compilation works without an installed .NET SDK.
#
# The total size is ~12-15 MB.  These DLLs ship with the application
# (added to the PyInstaller spec / build_release as data files).

from __future__ import annotations

import io
import os
import sys
import zipfile
from urllib.request import urlopen, Request

ROSLYN_VERSION = "4.12.0"

PACKAGES: list[tuple[str, str, str]] = [
    ("Microsoft.CodeAnalysis.Common", ROSLYN_VERSION, "netstandard2.0"),
    ("Microsoft.CodeAnalysis.CSharp", ROSLYN_VERSION, "netstandard2.0"),
    ("Microsoft.CodeAnalysis.Analyzers", "3.11.0", ""),
    ("System.Collections.Immutable", "8.0.0", "net6.0"),
    ("System.Reflection.Metadata", "8.0.0", "net6.0"),
    ("System.Runtime.CompilerServices.Unsafe", "6.0.0", "net6.0"),
    ("System.Memory", "4.6.0", "net6.0"),
    ("System.Buffers", "4.6.0", "net6.0"),
    ("System.Numerics.Vectors", "4.6.0", "net6.0"),
    ("System.Threading.Tasks.Extensions", "4.6.0", "net6.0"),
    ("System.Text.Encoding.CodePages", "8.0.0", "net6.0"),
]

NUGET_URL = "https://api.nuget.org/v3-flatcontainer/{pkg_lower}/{version}/{pkg_lower}.{version}.nupkg"

TARGET_DIR = os.path.join(os.path.dirname(__file__), "roslyn")


def _best_lib_dir(zf: zipfile.ZipFile, preferred_tfm: str) -> str:
    # Find the best lib/ subfolder inside a .nupkg zip.
    lib_dirs: list[str] = []
    for info in zf.infolist():
        parts = info.filename.replace("\\", "/").split("/")
        if len(parts) >= 3 and parts[0] == "lib" and parts[2].endswith(".dll"):
            tfm = parts[1]
            if tfm not in [d for d, _ in [(dd, None) for dd in lib_dirs]]:
                lib_dirs.append(tfm)
    if not lib_dirs:
        return ""
    if preferred_tfm and preferred_tfm in lib_dirs:
        return preferred_tfm
    priority = ["net8.0", "net7.0", "net6.0", "netstandard2.1", "netstandard2.0",
                "netcoreapp3.1", "net472", "net461"]
    for p in priority:
        if p in lib_dirs:
            return p
    return lib_dirs[0]


def fetch(target_dir: str | None = None, version: str | None = None) -> list[str]:
    # Download Roslyn NuGet packages and extract DLLs.
    #
    # Returns a list of extracted file paths.
    dest = target_dir or TARGET_DIR
    os.makedirs(dest, exist_ok=True)
    extracted: list[str] = []

    for pkg_name, pkg_version, preferred_tfm in PACKAGES:
        if version and pkg_name.startswith("Microsoft.CodeAnalysis"):
            pkg_version = version

        url = NUGET_URL.format(pkg_lower=pkg_name.lower(), version=pkg_version)
        print(f"  Downloading {pkg_name} {pkg_version} ...")
        req = Request(url, headers={"User-Agent": "SAO-UI/fetch_roslyn"})
        try:
            with urlopen(req, timeout=60) as resp:
                data = resp.read()
        except Exception as exc:
            print(f"    SKIP ({exc})")
            continue

        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            tfm = _best_lib_dir(zf, preferred_tfm)
            if not tfm:
                analyzers_prefix = "analyzers/dotnet/cs/"
                for info in zf.infolist():
                    norm = info.filename.replace("\\", "/")
                    if not norm.startswith(analyzers_prefix) or not norm.endswith(".dll"):
                        continue
                    rel = norm[len(analyzers_prefix):]
                    if "/" in rel:
                        continue
                    fname = os.path.basename(norm)
                    out_path = os.path.join(dest, fname)
                    with zf.open(info) as src, open(out_path, "wb") as dst:
                        dst.write(src.read())
                    extracted.append(out_path)
                    print(f"    -> {fname}")
                continue

            prefix = f"lib/{tfm}/"
            for info in zf.infolist():
                norm = info.filename.replace("\\", "/")
                if not norm.startswith(prefix):
                    continue
                if not (norm.endswith(".dll") or norm.endswith(".xml")):
                    continue
                rel = norm[len(prefix):]
                if "/" in rel:
                    continue
                fname = os.path.basename(norm)
                out_path = os.path.join(dest, fname)
                with zf.open(info) as src, open(out_path, "wb") as dst:
                    dst.write(src.read())
                if fname.endswith(".dll"):
                    extracted.append(out_path)
                    print(f"    -> {fname} ({tfm})")

    print(f"\nDone. {len(extracted)} DLLs in {os.path.abspath(dest)}")
    return extracted


def main() -> None:
    print(f"Fetching Roslyn {ROSLYN_VERSION} NuGet packages ...\n")
    fetch()


if __name__ == "__main__":
    main()
