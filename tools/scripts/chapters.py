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
    tools/scripts/chapters.py --force    check every tag, cache or not

A tag whose sources, tools, tests and chapter bundles are the ones of a
run that passed is skipped, because the result cannot differ. The file
build/chapters-verified.txt holds what passed. A change under docs/ or in
CLAUDE.md therefore costs no build.

The build of the check takes the tool paths from build/CMakeCache.txt.
Nothing is pushed. After the first public release the tags freeze, and
the rebuild no longer runs.
"""
import hashlib
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


# DESIGN: a tag is built and tested to prove that it builds and passes.
# That rests on the files the build and the tests read. Those are the
# sources, the tools, the tests, and the chapter bundles that the excerpt
# tests quote. A change under docs/ or in CLAUDE.md reaches none of them
# and cannot alter the result. A tag whose inputs are the ones of a run
# that passed is therefore skipped.
IGNORED = ("docs/", "CLAUDE.md", "README.md")
VERIFIED = os.path.join(ROOT, "build", "chapters-verified.txt")


def inputs_digest(tag):
    """The digest of the files of tag that the build and the tests read."""
    listing = git("ls-tree", "-r", tag)
    kept = [line for line in listing.split("\n")
            if not line.split("\t", 1)[-1].startswith(IGNORED)]
    return hashlib.sha256("\n".join(kept).encode("utf-8")).hexdigest()


def verified():
    """The digest that each tag had when it last passed."""
    found = {}
    if os.path.exists(VERIFIED):
        with open(VERIFIED, encoding="utf-8") as file:
            for line in file:
                parts = line.split()
                if len(parts) == 2:
                    found[parts[0]] = parts[1]
    return found


def record(passed):
    os.makedirs(os.path.dirname(VERIFIED), exist_ok=True)
    with open(VERIFIED, "w", encoding="utf-8") as file:
        for tag in sorted(passed):
            file.write("%s %s\n" % (tag, passed[tag]))


def code_digest(tag):
    """The digest of everything in tag outside the book and the docs."""
    listing = git("ls-tree", "-r", tag)
    kept = [line for line in listing.split("\n")
            if not line.split("\t", 1)[-1].startswith(IGNORED + (BOOK + "/",))]
    return hashlib.sha256("\n".join(kept).encode("utf-8")).hexdigest()


def run(step, tag):
    """Run one step of the check and stop the script when it fails."""
    result = subprocess.run(step, capture_output=True, text=True)
    output = result.stdout + result.stderr
    if result.returncode != 0 or (step[1] == "--build" and "warning:" in output):
        # The disabled tests of later chapters would hide the failure.
        shown = "\n".join(line for line in output.split("\n")
                          if "Disabled" not in line)
        sys.exit("%s: %s failed\n%s" % (tag, " ".join(step[:2]), shown[-4000:]))
    return output


def check(numbers, force=False):
    """Build the tags and run the tests of each one. Stop at a failure.

    DESIGN: every tag holds the same sources, tools and tests, and differs
    only in the chapter bundles it carries. The compiler of chapter 3 is
    therefore the compiler of chapter 25, and building it once serves
    every tag. The check exports the newest tag, builds it, and then walks
    the tags downwards, removing the bundles of the later chapters and
    configuring again. Nothing recompiles, because no source file changes,
    and each tag still configures and runs its own tests. A tag whose
    sources differ from the newest one is built on its own.
    """
    defines = ["-D%s=%s" % item for item in sorted(cache_values().items())]
    jobs = str(os.cpu_count() or 4)
    passed = {} if force else verified()
    todo = []
    for number in numbers:
        tag = "chapter-%d" % number
        if passed.get(tag) == inputs_digest(tag):
            print("%s unchanged since it passed" % tag)
        else:
            todo.append(number)
    if not todo:
        return

    newest = max(todo)
    shared = [n for n in todo if code_digest("chapter-%d" % n)
              == code_digest("chapter-%d" % newest)]
    alone = [n for n in todo if n not in shared]

    work = tempfile.mkdtemp(prefix="chapters-")
    source = os.path.join(work, "src")
    build = os.path.join(work, "build")
    os.makedirs(source)
    archive = subprocess.run(["git", "-C", ROOT, "archive", "chapter-%d" % newest],
                             capture_output=True, check=True)
    subprocess.run(["tar", "-x", "-f", "-", "-C", source], input=archive.stdout,
                   check=True)
    run(["cmake", "-S", source, "-B", build] + defines, "chapter-%d" % newest)
    run(["cmake", "--build", build, "-j", jobs], "chapter-%d" % newest)
    for number in sorted(shared, reverse=True):
        tag = "chapter-%d" % number
        for bundle in sorted(os.listdir(os.path.join(source, BOOK))):
            if re.match(r"^[0-9][0-9]-", bundle) and int(bundle[:2]) > number:
                shutil.rmtree(os.path.join(source, BOOK, bundle))
        run(["cmake", "-S", source, "-B", build] + defines, tag)
        output = run(["ctest", "--test-dir", build, "-j", jobs,
                      "--output-on-failure"], tag)
        summary = re.search(r"\d+% tests passed.*", output)
        print("%s %s" % (tag, summary.group(0) if summary else "passed"))
        passed[tag] = inputs_digest(tag)
        record(passed)
    shutil.rmtree(work)

    for number in sorted(alone):
        tag = "chapter-%d" % number
        work = tempfile.mkdtemp(prefix=tag + "-")
        source = os.path.join(work, "src")
        build = os.path.join(work, "build")
        os.makedirs(source)
        archive = subprocess.run(["git", "-C", ROOT, "archive", tag],
                                 capture_output=True, check=True)
        subprocess.run(["tar", "-x", "-f", "-", "-C", source],
                       input=archive.stdout, check=True)
        run(["cmake", "-S", source, "-B", build] + defines, tag)
        run(["cmake", "--build", build, "-j", jobs], tag)
        output = run(["ctest", "--test-dir", build, "-j", jobs,
                      "--output-on-failure"], tag)
        summary = re.search(r"\d+% tests passed.*", output)
        print("%s %s, built on its own" % (tag, summary.group(0) if summary
                                           else "passed"))
        passed[tag] = inputs_digest(tag)
        record(passed)
        shutil.rmtree(work)


def main():
    force = "--force" in sys.argv[1:]
    if "--check" in sys.argv[1:]:
        numbers = sorted(int(t.split("-")[1]) for t in git("tag", "--list", "chapter-*").split())
    else:
        numbers = rebuild()
    check(numbers, force)
    return 0


if __name__ == "__main__":
    sys.exit(main())
