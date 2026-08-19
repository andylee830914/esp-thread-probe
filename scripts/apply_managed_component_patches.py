#!/usr/bin/env python3
"""Apply local patches to ESP-IDF managed components."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def run_git_apply(args: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "apply", *args],
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def patch_state(managed_components: Path, patch: Path) -> str:
    check = run_git_apply(["--check", str(patch)], managed_components)
    if check.returncode == 0:
        return "pending"

    reverse_check = run_git_apply(["--reverse", "--check", str(patch)], managed_components)
    if reverse_check.returncode == 0:
        return "applied"

    details = (check.stderr or reverse_check.stderr).strip()
    raise RuntimeError(f"{patch.name} does not apply cleanly:\n{details}")


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", default=".", help="ESP-IDF project directory containing managed_components.")
    parser.add_argument(
        "--patch-dir",
        default="patches/managed_components",
        help="Directory containing managed component patch files.",
    )
    parser.add_argument("--check", action="store_true", help="Only verify patch state.")
    args = parser.parse_args()

    project_arg = Path(args.project)
    project_dir = project_arg.resolve() if project_arg.is_absolute() else (repo_root / project_arg).resolve()
    managed_components = project_dir / "managed_components"
    patch_dir = (repo_root / args.patch_dir).resolve()

    if not patch_dir.is_dir():
        print(f"missing patch directory: {patch_dir}", file=sys.stderr)
        return 2

    patches = sorted(patch_dir.glob("*.patch"))
    if not patches:
        print(f"no patches found in {patch_dir}", file=sys.stderr)
        return 2

    if not managed_components.is_dir():
        print(
            f"missing managed components: {managed_components}\n"
            "Run `idf.py update-dependencies` from the project directory first.",
            file=sys.stderr,
        )
        return 2

    failed = False
    for patch in patches:
        component_dir = managed_components / patch.stem
        if not component_dir.is_dir():
            print(f"{patch.name}: skipped, component is not present")
            continue

        try:
            state = patch_state(managed_components, patch)
        except RuntimeError as exc:
            print(exc, file=sys.stderr)
            failed = True
            continue

        if state == "applied":
            print(f"{patch.name}: already applied")
            continue

        if args.check:
            print(f"{patch.name}: pending")
            failed = True
            continue

        apply_result = run_git_apply([str(patch)], managed_components)
        if apply_result.returncode != 0:
            print(f"{patch.name}: apply failed\n{apply_result.stderr}", file=sys.stderr)
            failed = True
        else:
            print(f"{patch.name}: applied")

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
