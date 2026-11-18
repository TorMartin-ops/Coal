"""--changed: which suites are worth running for the paths touched since a base."""
import subprocess


def _git(repo, *args):
    p = subprocess.run(["git", "-C", repo] + list(args), capture_output=True, text=True)
    return p.stdout.split("\n") if p.returncode == 0 else []


def touched(repo, base=None):
    """Paths changed in the working tree and, with `base`, committed since the merge base with it."""
    paths = set(_git(repo, "diff", "--name-only", "HEAD")) | set(_git(repo, "ls-files", "--others", "--exclude-standard"))
    if base:
        mb = "".join(_git(repo, "merge-base", "HEAD", base)).strip()
        if mb:
            paths |= set(_git(repo, "diff", "--name-only", mb, "HEAD"))
    return sorted(p for p in paths if p)


def suites_for(paths, all_suites):
    hit = []
    for s in all_suites:
        if any(p == pre or p.startswith(pre.rstrip("/") + "/") for p in paths for pre in s.paths):
            hit.append(s)
    return hit
