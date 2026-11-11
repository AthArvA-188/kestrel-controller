"""Read and write KiCad S-expressions.

A parsed expression is a nested list. Bare tokens are `str`, quoted strings are
`Q`, and numbers stay as their source text so files round-trip unchanged.
"""


class Q(str):
    """A string that KiCad wrote in double quotes."""


def parse(text):
    pos = 0
    n = len(text)
    stack = [[]]
    while pos < n:
        c = text[pos]
        if c in " \t\r\n":
            pos += 1
        elif c == "(":
            stack.append([])
            pos += 1
        elif c == ")":
            done = stack.pop()
            stack[-1].append(done)
            pos += 1
        elif c == '"':
            pos += 1
            out = []
            while text[pos] != '"':
                if text[pos] == "\\":
                    out.append(text[pos:pos + 2])
                    pos += 2
                else:
                    out.append(text[pos])
                    pos += 1
            pos += 1
            stack[-1].append(Q("".join(out)))
        else:
            start = pos
            while pos < n and text[pos] not in ' \t\r\n()"':
                pos += 1
            stack[-1].append(text[start:pos])
    if len(stack) != 1 or len(stack[0]) != 1:
        raise ValueError("unbalanced S-expression")
    return stack[0][0]


def _atom(a):
    if isinstance(a, Q):
        return '"' + a + '"'
    if isinstance(a, float):
        s = ("%.6f" % a).rstrip("0").rstrip(".")
        return "0" if s in ("-0", "") else s
    return str(a)


def dump(node, indent=0):
    """Serialise with one child list per line, which is how KiCad 10 formats files."""
    pad = "\t" * indent
    if not isinstance(node, list):
        return pad + _atom(node)
    head = []
    i = 0
    while i < len(node) and not isinstance(node[i], list):
        head.append(_atom(node[i]))
        i += 1
    rest = node[i:]
    if not rest:
        return pad + "(" + " ".join(head) + ")"
    lines = [pad + "(" + " ".join(head)]
    for child in rest:
        lines.append(dump(child, indent + 1))
    lines.append(pad + ")")
    return "\n".join(lines)


def find(node, name):
    """First child list whose head is `name`, or None."""
    for child in node:
        if isinstance(child, list) and child and child[0] == name:
            return child
    return None


def find_all(node, name):
    return [c for c in node if isinstance(c, list) and c and c[0] == name]
