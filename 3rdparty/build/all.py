#!/usr/bin/env python3

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


# libtiff requires zlib, libjpeg-turbo, libwebp, and zstd prefixes.
# The other recipes either have no build-order dependency or build their
# private dependencies themselves.
BUILD_ORDER = (
    "stb",
    "zlib",
    "libjpeg-turbo",
    "libwebp",
    "zstd",
    "libspng",
    "libtiff",
    "libgif",
    "openjpeg",
    "openexr",
    "libavif",
    "libheif",
    "libjxl",
    "mediainfo",
)


def configure_git_patch():
    if sys.platform != "win32":
        return None

    git = shutil.which("git")
    if git is None:
        raise RuntimeError("git was not found in PATH.")

    git_path = Path(git).resolve()
    for directory in (git_path.parent, *git_path.parents):
        patch = directory / "usr" / "bin" / "patch.exe"
        if not patch.is_file():
            continue

        patch_directory = str(patch.parent)
        path_entries = os.environ.get("PATH", "").split(os.pathsep)
        path_entries = [
            entry
            for entry in path_entries
            if entry and os.path.normcase(entry) != os.path.normcase(patch_directory)
        ]
        os.environ["PATH"] = os.pathsep.join([patch_directory, *path_entries])
        return patch

    raise RuntimeError(f"Git patch.exe was not found near {git_path}.")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Build every third-party library sequentially."
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Print each Perl recipe configuration without building.",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    script_directory = Path(__file__).resolve().parent
    perl = shutil.which("perl")
    if perl is None:
        print("[ERROR] perl was not found in PATH.", file=sys.stderr)
        return 1

    try:
        git_patch = configure_git_patch()
    except RuntimeError as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        return 1

    if git_patch is not None:
        print(f"[INFO] Using Git patch: {git_patch}", flush=True)

    scripts = [
        script_directory / f"build_{library}.pl" for library in BUILD_ORDER
    ]
    missing = [script for script in scripts if not script.is_file()]
    if missing:
        for script in missing:
            print(f"[ERROR] Missing build script: {script}", file=sys.stderr)
        return 1

    action = "Checking" if args.check else "Building"
    total = len(scripts)
    for index, (library, script) in enumerate(
        zip(BUILD_ORDER, scripts), start=1
    ):
        print("\n" + "=" * 72, flush=True)
        print(f"[{index}/{total}] {action} {library}", flush=True)
        print("=" * 72, flush=True)

        command = [perl, str(script)]
        if args.check:
            command.append("--print-config")

        try:
            completed = subprocess.run(command, cwd=script_directory)
        except KeyboardInterrupt:
            print(f"\n[STOPPED] Interrupted while processing {library}.", file=sys.stderr)
            return 130

        if completed.returncode != 0:
            print(
                f"\n[FAILED] {library} exited with code {completed.returncode}.",
                file=sys.stderr,
            )
            return completed.returncode

    if args.check:
        print("\n[DONE] All build configurations are valid.")
    else:
        print("\n[DONE] All third-party libraries were built successfully.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
