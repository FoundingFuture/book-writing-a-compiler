#!/usr/bin/env python3
"""Write tests/excerpts.txt, the code excerpts of the book that ctest checks.

Each C, CMake or Anti fence of a chapter that appears verbatim in a
repository file becomes one line. The files are the sources of antic, its
runtime, its tests and tools, and the early programs in the chapter
folders. The line holds the chapter bundle, the language group and the
number of the fence within that group. It ends with the file and a label
taken from the first line of the fence.

A C or CMake fence that no file holds is printed and left out, and the
exit code says so. An Anti fence is printed the same way and does not
fail, because a chapter shows many short forms that no program holds.

    tools/scripts/excerpts.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOURCES = ("src", "rt", "std", "libs", "tests", "tools", "writing-a-compiler",
           "CMakeLists.txt")


def repository_files():
    files = []
    for top in SOURCES:
        path = os.path.join(ROOT, top)
        if os.path.isfile(path):
            files.append(top)
            continue
        for directory, _, names in os.walk(path):
            for name in sorted(names):
                if name.endswith((".c", ".h", ".cmake", ".txt", ".sh", ".py",
                                  ".anti")):
                    files.append(os.path.relpath(os.path.join(directory, name), ROOT))
    return sorted(files)


def label(line):
    return re.sub(r"[^A-Za-z0-9_ ]", "", line)[:40].strip()


def main():
    contents = {}
    for name in repository_files():
        try:
            with open(os.path.join(ROOT, name), encoding="utf-8") as source:
                contents[name] = source.read()
        except UnicodeDecodeError:
            continue
    book = os.path.join(ROOT, "writing-a-compiler")
    lines = []
    missing = []
    loose = []
    for bundle in sorted(os.listdir(book)):
        page = os.path.join(book, bundle, "index.md")
        if not os.path.isfile(page):
            continue
        with open(page, encoding="utf-8") as chapter:
            text = chapter.read()
        counts = {"code": 0, "anti": 0}
        for match in re.finditer(r"^```(c|cmake|anti)\n(.*?)^```\n", text,
                                 re.S | re.M):
            group = "anti" if match.group(1) == "anti" else "code"
            counts[group] += 1
            index = counts[group]
            body = match.group(2)
            found = next((name for name, data in contents.items() if body in data), None)
            if found is None:
                where = "%s %s fence %d: %s" % (bundle, group, index,
                                                body.split("\n")[0])
                if group == "code":
                    missing.append(where)
                else:
                    loose.append(where)
                continue
            lines.append("%s|%s|%d|%s|%s" % (bundle, group, index, found,
                                             label(body.split("\n")[0])))
    with open(os.path.join(ROOT, "tests", "excerpts.txt"), "w", encoding="utf-8") as out:
        out.write("\n".join(lines) + "\n")
    for entry in loose:
        print("no file holds: " + entry)
    for entry in missing:
        print("not in the repository: " + entry)
    # DESIGN: a fence that no file holds is dropped from the index, and a
    # dropped fence is a test that stops running. The exit code says so,
    # and the test excerpts_complete fails on it.
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
