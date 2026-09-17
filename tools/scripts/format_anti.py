#!/usr/bin/env python3
"""Rewrite Anti source into the formatter style of docs/decisions.md.

The style has one tab per indentation level and a wrapped line one level
deeper, with nothing aligned past the indent. An item body opens its brace
on its own line. A statement block opens its brace on the line of the
statement, with `} else {` and `} while cond` on one line. Parentheses
around a whole condition are dropped, and each statement has its own line.

The script stands in for `anti fmt` until the tool exists. It formats
.anti files in place, and the ```anti fences of Markdown files with four
spaces per level, the form that `anti html` renders.

    tools/scripts/format_anti.py [--check] FILE [FILE]
"""
import os
import re
import sys

# Sources that hold a syntax error on purpose. The script has no parser, so
# it leaves them and the listings that show them as they are.
UNPARSED = ("tests/errors/syntax.anti",)
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def unparsed_listings():
    listings = set()
    for name in UNPARSED:
        with open(os.path.join(ROOT, name), encoding="utf-8") as source:
            listings.add(source.read().replace("\t", "    "))
    return listings

ITEM_WORDS = {"fn", "struct", "union"}
ITEM_PREFIX = {"pub", "export", "packed"}
BLOCK_WORDS = {"if", "else", "while", "do"}
OPERATORS = ("<<=", ">>=", "...", "->", "..", "==", "!=", "<=", ">=", "&&",
             "||", "<<", ">>", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=")


class Token:
    def __init__(self, kind, text, gap, newline, blank):
        self.kind = kind          # word, string, punct, line_comment, block_comment
        self.text = text
        self.gap = gap            # the spaces before it on its source line
        self.newline = newline    # a line break comes before it
        self.blank = blank        # an empty line comes before it


def tokenize(src):
    tokens = []
    i = 0
    n = len(src)
    newline = False
    blank = False
    gap = ""
    while i < n:
        c = src[i]
        if c == "\n":
            blank = newline
            newline = True
            gap = ""
            i += 1
            continue
        if c in " \t\r":
            gap += " "
            i += 1
            continue
        start = i
        m = re.match(r'(br|r|b)?(#*)"', src[i:])
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            kind = "line_comment"
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            kind = "block_comment"
        elif m and (m.group(1) or m.group(2) or src[i] == '"'):
            hashes = m.group(2)
            raw = (m.group(1) or "").endswith("r")
            j = i + m.end()
            while j < n:
                if not raw and src[j] == "\\":
                    j += 2
                    continue
                if src[j] == '"' and src.startswith(hashes, j + 1):
                    j += 1 + len(hashes)
                    break
                j += 1
            i = j
            kind = "string"
        elif c == "'":
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == "\\" else 1
            i = j + 1
            kind = "string"
        elif c.isalnum() or c == "_":
            fm = re.match(r"[0-9][0-9_]*\.[0-9][0-9_]*([eE][+-]?[0-9_]+)?", src[i:])
            if fm:
                i += fm.end()
            else:
                while i < n and (src[i].isalnum() or src[i] == "_"):
                    i += 1
            kind = "word"
        else:
            for op in OPERATORS:
                if src.startswith(op, i):
                    i += len(op)
                    break
            else:
                i += 1
            kind = "punct"
        tokens.append(Token(kind, src[start:i], " " if gap else "", newline,
                            blank))
        newline = False
        blank = False
        gap = ""
    return tokens


class Formatter:
    def __init__(self, unit):
        self.unit = unit
        self.out = []
        self.line = []        # (token, space before) of the current line
        self.depth = 0
        self.extra = 0        # 1 for a wrapped line

    def emit(self, token, space):
        self.line.append((token, space and bool(self.line)))

    def flush(self):
        if self.line:
            text = "".join((" " if space else "") + t.text
                           for t, space in self.line)
            self.out.append(self.unit * (self.depth + self.extra) + text)
        self.line = []

    def blank(self):
        if self.out and self.out[-1] != "":
            self.out.append("")


def drop_condition_parens(line):
    """Drop one pair of parentheses around the whole condition of an if or
    while that the line holds."""
    for start, (tok, _) in enumerate(line):
        if tok.text not in ("if", "while") or start + 1 >= len(line) or \
                line[start + 1][0].text != "(":
            continue
        end = len(line) - 1
        if line[end][0].text == "do":
            end -= 1
        if line[end][0].text != ")":
            return
        level = 0
        for k in range(start + 1, end + 1):
            if line[k][0].text == "(":
                level += 1
            elif line[k][0].text == ")":
                level -= 1
                if level == 0 and k != end:
                    return
        del line[end]
        del line[start + 1]
        if start + 1 < len(line):
            line[start + 1] = (line[start + 1][0], True)
        return


def format_code(src, unit):
    toks = tokenize(src)
    f = Formatter(unit)
    stack = []            # item, block or literal for each open brace
    closed = []           # the kind of the brace each closed block opened with
    parens = 0
    statement = []        # the tokens of the current statement
    do_open = []          # for each open block, whether it opened with do
    after_do = False
    for index, tok in enumerate(toks):
        in_literal = bool(stack) and stack[-1] == "literal"
        at_statement_level = parens == 0 and not in_literal
        if tok.newline and f.line:
            if tok.kind == "line_comment":
                pass
            elif at_statement_level and after_do:
                f.flush()
                after_do = False
                statement = []
            else:
                f.flush()
                if statement:
                    f.extra = 1
        if tok.blank and not f.line and not in_literal and parens == 0:
            f.blank()
        if tok.kind == "line_comment":
            if not tok.newline and not f.line and f.out and f.out[-1] != "":
                f.out[-1] += " " + tok.text
                continue
            if tok.newline or not f.line:
                f.flush()
                f.extra = 0 if not statement else f.extra
                f.emit(tok, False)
            else:
                f.emit(tok, True)
            f.flush()
            continue
        if tok.kind == "block_comment":
            if f.line and not tok.newline:
                f.emit(tok, True)
            else:
                f.flush()
                first, *rest = tok.text.split("\n")
                f.out.append(unit * f.depth + first)
                f.out.extend(rest)
            continue
        if tok.kind == "punct" and tok.text == "{":
            words = [t.text for t in statement if t.kind == "word"]
            lead = [w for w in words if w not in ITEM_PREFIX]
            first = statement[0].text if statement else None
            if at_statement_level and not stack and lead and \
                    lead[0] in ITEM_WORDS:
                kind = "item"
            elif at_statement_level and (first is None or first in BLOCK_WORDS
                                         or first == "}"):
                kind = "block"
            else:
                kind = "literal"
            if kind == "literal":
                f.emit(tok, tok.gap != "")
                statement.append(tok)
                stack.append(kind)
                parens += 0
                continue
            drop_condition_parens(f.line)
            if kind == "item":
                f.flush()
                f.extra = 0
                f.emit(tok, False)
            else:
                f.emit(tok, True)
            f.flush()
            f.extra = 0
            do_open.append(bool(statement) and statement[0].text == "do" or
                           (len(statement) > 1 and statement[-1].text == "do"
                            and statement[0].text == "}"))
            f.depth += 1
            stack.append(kind)
            statement = []
            continue
        if tok.kind == "punct" and tok.text == "}":
            kind = stack.pop() if stack else "block"
            if kind == "literal":
                f.emit(tok, tok.gap != "")
                statement.append(tok)
                continue
            if kind == "item" and f.line and f.line[-1][0].text not in (",", ";"):
                if any(t.text == ":" for t, _ in f.line):
                    f.emit(Token("punct", ",", "", False, False), False)
            f.flush()
            f.extra = 0
            f.depth -= 1
            opened_with_do = do_open.pop() if do_open else False
            f.emit(tok, False)
            following = toks[index + 1].text if index + 1 < len(toks) else None
            if following == "else" or (following == "while" and opened_with_do):
                after_do = following == "while"
                statement = [tok]
            else:
                f.flush()
                statement = []
                # Items are separated by an empty line.
                if kind == "item" and index + 1 < len(toks):
                    f.blank()
            continue
        if tok.kind == "punct" and tok.text in ("(", "["):
            parens += 1
        if tok.kind == "punct" and tok.text in (")", "]"):
            parens -= 1
        f.emit(tok, tok.gap != "" and not tok.newline or
               (tok.newline and bool(f.line)))
        statement.append(tok)
        if tok.kind == "punct" and tok.text == ";" and parens == 0 and \
                not in_literal:
            f.flush()
            f.extra = 0
            statement = []
        elif tok.kind == "punct" and tok.text == "," and parens == 0 and \
                stack and stack[-1] == "item":
            f.flush()
            f.extra = 0
            statement = []
    f.flush()
    lines = [line.rstrip() for line in f.out]
    while lines and lines[-1] == "":
        lines.pop()
    while lines and lines[0] == "":
        lines.pop(0)
    return "\n".join(lines) + "\n" if lines else ""


def format_markdown(text):
    skipped = unparsed_listings()
    out = []
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        out.append(lines[i])
        if lines[i].strip() == "```anti":
            j = i + 1
            while j < len(lines) and lines[j].strip() != "```":
                j += 1
            body = "\n".join(lines[i + 1:j]) + "\n"
            formatted = body.rstrip("\n") if body in skipped else \
                format_code(body, "    ").rstrip("\n")
            if formatted:
                out.extend(formatted.split("\n"))
            if j < len(lines):
                out.append(lines[j])
            i = j + 1
            continue
        i += 1
    return "\n".join(out)


def main(argv):
    check = "--check" in argv
    changed = []
    for path in [a for a in argv if a != "--check"]:
        with open(path, encoding="utf-8") as source:
            original = source.read()
        if os.path.abspath(path) in [os.path.join(ROOT, u) for u in UNPARSED]:
            result = original
        elif path.endswith(".md"):
            result = format_markdown(original)
        else:
            result = format_code(original, "\t")
        if result != original:
            changed.append(path)
            if not check:
                with open(path, "w", encoding="utf-8") as target:
                    target.write(result)
    for path in changed:
        print(path)
    return 1 if check and changed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
