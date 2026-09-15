#!/usr/bin/env python3
"""Rebuild the chapter commits from main, tag them and test every tag.

The series starts at a root commit and has one commit per chapter
bundle, in chapter order. The commit for chapter N holds the tree of main
without the bundles of later chapters. It therefore holds the complete
code with the book and the tests of chapters 1 to N. The last commit has
the tree of main, and main moves to it. Each published chapter gets the
tag chapter-N. The last step exports every tag in order into a new
directory, builds it and runs ctest. It stops at the first failure.

    tools/scripts/chapters.py            rebuild, tag and check
    tools/scripts/chapters.py --check    check the existing tags only

The build of the check takes the tool paths from build/CMakeCache.txt.
Nothing is pushed. After the first public release the tags freeze, and
the rebuild no longer runs.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BOOK = "writing-a-compiler"
CACHE_KEYS = ("ANTIC_LLVM_MC", "ANTIC_LLVM_AR", "ANTIC_RAYLIB_DIR", "ANTIC_SYSROOT_DIR")


def git(*args, env=None, stdin=None):
    result = subprocess.run(["git", "-C", ROOT] + list(args), input=stdin,
                            capture_output=True, text=True, env=env)
    if result.returncode != 0:
        sys.exit("git %s failed:\n%s" % (" ".join(args), result.stderr))
    return result.stdout.strip()


def chapters(commit):
    """The chapter bundles of commit as (number, bundle, title, draft)."""
    found = []
    for name in git("ls-tree", "--name-only", commit, BOOK + "/").split("\n"):
        bundle = name.split("/")[-1]
        if not re.match(r"^[0-9][0-9]-", bundle):
            continue
        page = git("show", "%s:%s/%s/index.md" % (commit, BOOK, bundle))
        title = re.search(r'^title: "(.*)"$', page, re.M).group(1)
        draft = re.search(r"^draft: true$", page, re.M) is not None
        found.append((int(bundle[:2]), bundle, title, draft))
    return sorted(found)


def tree_without(commit, bundles):
    """The tree of commit without the given chapter bundles."""
    index = tempfile.NamedTemporaryFile(delete=False)
    index.close()
    env = dict(os.environ, GIT_INDEX_FILE=index.name)
    try:
        git("read-tree", commit, env=env)
        for bundle in bundles:
            git("rm", "--cached", "-r", "-q", "%s/%s" % (BOOK, bundle), env=env)
        return git("write-tree", env=env)
    finally:
        os.unlink(index.name)


def rebuild():
    if git("status", "--porcelain", "--untracked-files=no"):
        sys.exit("the working tree has changes; commit them first")
    if git("rev-parse", "--abbrev-ref", "HEAD") != "main":
        sys.exit("run the script on main")
    head = git("rev-parse", "HEAD")
    series = chapters(head)
    parent = None
    tags = []
    for number, bundle, title, draft in series:
        later = [b for n, b, _, _ in series if n > number]
        tree = tree_without(head, later)
        message = ("Chapter %d, %s\n\nThe tree holds the complete code of main with the book "
                   "and the tests of chapters 1 to %d.\n" % (number, title, number))
        args = ["commit-tree", tree, "-F", "-"]
        if parent is not None:
            args += ["-p", parent]
        parent = git(*args, stdin=message)
        if not draft:
            tags.append((number, parent))
        print("chapter %2d %s %s%s" % (number, parent[:7], bundle, " (draft)" if draft else ""))
    if git("rev-parse", parent + "^{tree}") != git("rev-parse", head + "^{tree}"):
        sys.exit("the last chapter commit does not hold the tree of main")
    for number, commit in tags:
        git("tag", "-f", "chapter-%d" % number, commit)
    git("update-ref", "refs/heads/main", parent, head)
    return [number for number, _ in tags]


def cache_values():
    values = {}
    path = os.path.join(ROOT, "build", "CMakeCache.txt")
    with open(path, encoding="utf-8") as cache:
        for line in cache:
            key = line.split(":", 1)[0]
            if key in CACHE_KEYS:
                values[key] = line.split("=", 1)[1].strip()
    return values


def check(numbers):
    """Export each tag, build it and run its tests. Stop at a failure."""
    defines = ["-D%s=%s" % item for item in sorted(cache_values().items())]
    jobs = str(os.cpu_count() or 4)
    for number in numbers:
        tag = "chapter-%d" % number
        work = tempfile.mkdtemp(prefix=tag + "-")
        source = os.path.join(work, "src")
        build = os.path.join(work, "build")
        os.makedirs(source)
        archive = subprocess.run(["git", "-C", ROOT, "archive", tag], capture_output=True, check=True)
        subprocess.run(["tar", "-x", "-f", "-", "-C", source], input=archive.stdout, check=True)
        steps = [["cmake", "-S", source, "-B", build] + defines,
                 ["cmake", "--build", build, "-j", jobs],
                 ["ctest", "--test-dir", build, "-j", jobs, "--output-on-failure"]]
        for step in steps:
            result = subprocess.run(step, capture_output=True, text=True)
            output = result.stdout + result.stderr
            if result.returncode != 0 or (step[1] == "--build" and "warning:" in output):
                # The disabled tests of later chapters would hide the failure.
                shown = "\n".join(line for line in output.split("\n")
                                  if "Disabled" not in line)
                sys.exit("%s: %s failed\n%s" % (tag, " ".join(step[:2]), shown[-4000:]))
        summary = re.search(r"\d+% tests passed.*", output)
        print("%s %s" % (tag, summary.group(0) if summary else "passed"))
        shutil.rmtree(work)


def main():
    if "--check" in sys.argv[1:]:
        numbers = sorted(int(t.split("-")[1]) for t in git("tag", "--list", "chapter-*").split())
    else:
        numbers = rebuild()
    check(numbers)
    return 0


if __name__ == "__main__":
    sys.exit(main())
