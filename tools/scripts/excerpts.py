#!/usr/bin/env python3
"""Write tests/excerpts.txt, the code excerpts of the book that ctest checks.

Each C or CMake fence of a chapter that appears verbatim in a repository
file becomes one line. The files are the sources of antic, its runtime, its
tests and tools, and the early programs in the chapter folders. The line holds the chapter bundle and the number of
the fence among the chapter's C and CMake fences. It ends with the file and
a label taken from the first line of the fence. A fence that no file holds
is printed and left out.

    tools/scripts/excerpts.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOURCES = ("src", "rt", "std", "tests", "tools", "writing-a-compiler",
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
                if name.endswith((".c", ".h", ".cmake", ".txt", ".sh", ".py")):
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
    for bundle in sorted(os.listdir(book)):
        page = os.path.join(book, bundle, "index.md")
        if not os.path.isfile(page):
            continue
        with open(page, encoding="utf-8") as chapter:
            text = chapter.read()
        index = 0
        for match in re.finditer(r"^```(c|cmake)\n(.*?)^```\n", text, re.S | re.M):
            index += 1
            body = match.group(2)
            found = next((name for name, data in contents.items() if body in data), None)
            if found is None:
                missing.append("%s fence %d: %s" % (bundle, index, body.split("\n")[0]))
                continue
            lines.append("%s|%d|%s|%s" % (bundle, index, found, label(body.split("\n")[0])))
    with open(os.path.join(ROOT, "tests", "excerpts.txt"), "w", encoding="utf-8") as out:
        out.write("\n".join(lines) + "\n")
    for entry in missing:
        print("not in the repository: " + entry)
    return 0


if __name__ == "__main__":
    sys.exit(main())
