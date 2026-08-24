#!/usr/bin/env python3
"""Compute the next Matrix Media Share Client app version.

Versions are year.month.day.build in America/Chicago, with unpadded month and
day. The first build on a calendar day is .1; later builds that same day
increment the last component. A new day always starts at .1 and never continues
the previous release's date.

Examples:
  2026.8.24.1
  2026.8.24.2

Default: print the next version to stdout.
--write: update VERSION.txt in the repository root.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from datetime import date, datetime
from pathlib import Path
from zoneinfo import ZoneInfo

TZ = ZoneInfo("America/Chicago")
VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)\.(\d+)$")


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def format_version(year: int, month: int, day: int, build: int) -> str:
    return f"{year}.{month}.{day}.{build}"


def _parse_unpadded_parts(text: str) -> tuple[int, int, int, int] | None:
    match = VERSION_RE.fullmatch(text.strip())
    if match is None:
        return None
    parts = match.groups()
    if any(part != str(int(part)) for part in parts):
        return None
    return tuple(int(part) for part in parts)  # type: ignore[return-value]


def parse_version(text: str) -> tuple[int, int, int, int] | None:
    return _parse_unpadded_parts(text)


def parse_tag(text: str) -> tuple[int, int, int, int] | None:
    stripped = text.strip()
    if not stripped.startswith("v"):
        return None
    return _parse_unpadded_parts(stripped[1:])


def chicago_today(now: datetime | None = None) -> date:
    current = now if now is not None else datetime.now(TZ)
    if current.tzinfo is None:
        current = current.replace(tzinfo=TZ)
    return current.astimezone(TZ).date()


def next_build(year: int, month: int, day: int, versions: list[tuple[int, int, int, int]]) -> int:
    builds = [build for y, m, d, build in versions if (y, m, d) == (year, month, day)]
    return max(builds, default=0) + 1


def next_version(year: int, month: int, day: int, versions: list[tuple[int, int, int, int]]) -> str:
    return format_version(year, month, day, next_build(year, month, day, versions))


def read_version_file(path: Path) -> tuple[int, int, int, int] | None:
    if not path.is_file():
        return None
    return parse_version(path.read_text(encoding="utf-8"))


def git_output(args: list[str], cwd: Path) -> list[str]:
    result = subprocess.run(
        args,
        cwd=cwd,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return []
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def collect_git_tags(root: Path) -> list[str]:
    tags = git_output(["git", "tag", "--list", "v*"], root)
    # Best-effort remote tags so a stale local clone does not reuse a released N.
    remote_refs = git_output(["git", "ls-remote", "--tags", "origin", "v*"], root)
    for ref in remote_refs:
        parts = ref.split()
        if len(parts) < 2:
            continue
        name = parts[-1]
        if name.endswith("^{}"):
            continue
        prefix = "refs/tags/"
        tags.append(name[len(prefix):] if name.startswith(prefix) else name)
    return tags


def collect_versions(root: Path) -> list[tuple[int, int, int, int]]:
    versions: list[tuple[int, int, int, int]] = []
    for tag in collect_git_tags(root):
        parsed = parse_tag(tag)
        if parsed is not None:
            versions.append(parsed)
    file_version = read_version_file(root / "VERSION.txt")
    if file_version is not None:
        versions.append(file_version)
    return versions


def compute_next(root: Path, today: date) -> str:
    return next_version(today.year, today.month, today.day, collect_versions(root))


def write_version(path: Path, version: str) -> None:
    path.write_text(f"{version}\n", encoding="utf-8")


def self_test() -> None:
    assert format_version(2026, 8, 24, 1) == "2026.8.24.1"
    assert parse_version("2026.8.24.1") == (2026, 8, 24, 1)
    assert parse_tag("v2026.8.24.2") == (2026, 8, 24, 2)
    assert parse_version("2026.08.24.1") is None  # padded month is rejected
    assert next_version(2026, 8, 24, []) == "2026.8.24.1"
    assert next_version(2026, 8, 24, [(2026, 3, 30, 3)]) == "2026.8.24.1"
    assert next_version(2026, 8, 24, [(2026, 8, 24, 1), (2026, 8, 24, 2)]) == "2026.8.24.3"
    assert next_version(2026, 3, 30, [(2026, 3, 30, 1), (2026, 3, 30, 2)]) == "2026.3.30.3"
    chicago = chicago_today(datetime(2026, 8, 24, 18, 53, tzinfo=ZoneInfo("UTC")))
    assert chicago == date(2026, 8, 24)
    utc_next_day = chicago_today(datetime(2026, 8, 25, 4, 30, tzinfo=ZoneInfo("UTC")))
    assert utc_next_day == date(2026, 8, 24)  # still 23:30 in Chicago


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--write",
        action="store_true",
        help="Write the computed version to VERSION.txt",
    )
    parser.add_argument(
        "--date",
        metavar="YYYY-MM-DD",
        help="Override the America/Chicago calendar date (for tests)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run built-in checks and exit",
    )
    args = parser.parse_args()

    if args.self_test:
        self_test()
        print("ok")
        return 0

    root = repo_root()
    if args.date:
        today = date.fromisoformat(args.date)
    else:
        today = chicago_today()

    version = compute_next(root, today)
    if args.write:
        write_version(root / "VERSION.txt", version)
    print(version)
    return 0


if __name__ == "__main__":
    sys.exit(main())
