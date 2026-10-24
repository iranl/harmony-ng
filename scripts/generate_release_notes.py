#!/usr/bin/env python3
"""
generate_release_notes.py - Automatically generate release notes from git commits since previous release.
"""

import os
import re
import sys
import argparse
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def run_git(args, cwd=REPO_ROOT):
    try:
        return subprocess.check_output(
            ["git"] + args, cwd=cwd, text=True, encoding="utf-8"
        ).strip()
    except subprocess.CalledProcessError:
        return ""


def get_repo_slug():
    env_repo = os.environ.get("GITHUB_REPOSITORY")
    if env_repo:
        return env_repo
    remote_url = run_git(["remote", "get-url", "origin"])
    m = re.search(r"github\.com[:/]([^/]+/[^/\.]+?)(?:\.git)?$", remote_url)
    if m:
        return m.group(1)
    return "iranl/harmony-ng"


def get_tags():
    out = run_git(["tag", "-l", "v*", "--sort=-v:refname"])
    if not out:
        return []
    return [t.strip() for t in out.splitlines() if t.strip()]


def parse_semver(tag_str):
    m = re.match(r"^v?(\d+)\.(\d+)\.(\d+)", tag_str)
    if m:
        return (int(m.group(1)), int(m.group(2)), int(m.group(3)))
    return None


def find_previous_tag(target_version, all_tags):
    target_tuple = parse_semver(target_version)
    if not target_tuple:
        return None
    valid_tags = []
    for tag in all_tags:
        ver_tuple = parse_semver(tag)
        if ver_tuple and ver_tuple < target_tuple:
            valid_tags.append((ver_tuple, tag))
    if not valid_tags:
        return None
    valid_tags.sort(key=lambda x: x[0], reverse=True)
    return valid_tags[0][1]


def get_commits_between(prev_tag, target_ref):
    if prev_tag:
        git_range = f"{prev_tag}..{target_ref}"
    else:
        git_range = target_ref

    out = run_git(["log", git_range, "--pretty=format:%H|%h|%s|%an", "--no-merges"])
    if not out:
        return []

    commits = []
    for line in out.splitlines():
        parts = line.split("|", 3)
        if len(parts) == 4:
            commits.append({
                "full_hash": parts[0],
                "short_hash": parts[1],
                "subject": parts[2],
                "author": parts[3],
            })
    return commits


def categorize_commits(commits):
    categories = {
        "Features": [],
        "Bug Fixes": [],
        "Performance Improvements": [],
        "Documentation": [],
        "Refactoring": [],
        "Maintenance & Tooling": [],
        "Other Changes": [],
    }

    cat_patterns = [
        ("Features", re.compile(r"^feat(\([^)]+\))?!?:", re.I)),
        ("Bug Fixes", re.compile(r"^fix(\([^)]+\))?!?:", re.I)),
        ("Performance Improvements", re.compile(r"^perf(\([^)]+\))?!?:", re.I)),
        ("Documentation", re.compile(r"^docs(\([^)]+\))?!?:", re.I)),
        ("Refactoring", re.compile(r"^refactor(\([^)]+\))?!?:", re.I)),
        ("Maintenance & Tooling", re.compile(r"^(chore|build|ci|tooling|test)(\([^)]+\))?!?:", re.I)),
    ]

    skip_pattern = re.compile(r"^chore\(release\):\s*bump version", re.I)

    for c in commits:
        subj = c["subject"]
        if skip_pattern.search(subj):
            continue

        matched = False
        for cat_name, pattern in cat_patterns:
            if pattern.search(subj):
                categories[cat_name].append(c)
                matched = True
                break
        if not matched:
            categories["Other Changes"].append(c)

    return categories


def generate_markdown(version, prev_tag, categories, repo_slug):
    norm_version = version.lstrip("v")
    lines = [f"### HarmonyNG Release v{norm_version}\n"]

    has_any = False
    for cat_name, items in categories.items():
        if not items:
            continue
        has_any = True
        lines.append(f"#### {cat_name}")
        for item in items:
            commit_url = f"https://github.com/{repo_slug}/commit/{item['full_hash']}"
            lines.append(f"* {item['subject']} ([{item['short_hash']}]({commit_url}))")
        lines.append("")

    if not has_any:
        lines.append("* No code changes recorded in this release.\n")

    target_tag = "v" + norm_version
    if prev_tag:
        compare_url = f"https://github.com/{repo_slug}/compare/{prev_tag}...{target_tag}"
        lines.append(f"**Full Changelog**: {compare_url}")

    return "\n".join(lines).strip() + "\n"


def main():
    parser = argparse.ArgumentParser(
        description="Generate markdown release notes from git commits."
    )
    parser.add_argument(
        "--version",
        default="",
        help="Target release version (e.g. 1.10.0 or v1.10.0). Defaults to VERSION file content.",
    )
    parser.add_argument(
        "--prev-tag",
        default="",
        help="Explicit previous release tag to diff against. Detected automatically if omitted.",
    )
    parser.add_argument(
        "--output",
        "-o",
        default="",
        help="Path to output markdown file (prints to stdout if omitted).",
    )
    parser.add_argument(
        "--repo",
        default="",
        help="GitHub repo slug (owner/repo). Detected automatically if omitted.",
    )
    args = parser.parse_args()

    version = args.version
    if not version:
        version_file = REPO_ROOT / "VERSION"
        if version_file.exists():
            version = version_file.read_text(encoding="utf-8").strip()
        else:
            version = "1.0.0"

    repo_slug = args.repo or get_repo_slug()
    all_tags = get_tags()

    prev_tag = args.prev_tag
    if not prev_tag:
        prev_tag = find_previous_tag(version, all_tags)

    target_tag = "v" + version.lstrip("v")
    # If the tag exists locally, diff against target_tag; otherwise diff against HEAD
    target_ref = target_tag if target_tag in all_tags else "HEAD"

    commits = get_commits_between(prev_tag, target_ref)
    categories = categorize_commits(commits)
    markdown = generate_markdown(version, prev_tag, categories, repo_slug)

    if args.output:
        out_path = Path(args.output)
        out_path.write_text(markdown, encoding="utf-8")
        print(f"[+] Release notes written to {out_path}")
    else:
        print(markdown)


if __name__ == "__main__":
    main()
