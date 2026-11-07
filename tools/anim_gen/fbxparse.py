"""Minimal binary-FBX reader (versions 7100-7700): node tree with zlib array support."""
import struct, zlib


class Node:
    __slots__ = ("name", "props", "children")

    def __init__(self, name):
        self.name, self.props, self.children = name, [], []

    def find(self, name):
        return [c for c in self.children if c.name == name]

    def first(self, name):
        for c in self.children:
            if c.name == name:
                return c
        return None


def _prop(buf, p):
    t = chr(buf[p]); p += 1
    if t == 'Y': return struct.unpack_from('<h', buf, p)[0], p + 2
    if t == 'C': return bool(buf[p]), p + 1
    if t == 'I': return struct.unpack_from('<i', buf, p)[0], p + 4
    if t == 'F': return struct.unpack_from('<f', buf, p)[0], p + 4
    if t == 'D': return struct.unpack_from('<d', buf, p)[0], p + 8
    if t == 'L': return struct.unpack_from('<q', buf, p)[0], p + 8
    if t in 'SR':
        n = struct.unpack_from('<I', buf, p)[0]; p += 4
        d = bytes(buf[p:p + n])
        return (d.decode('utf-8', 'replace') if t == 'S' else d), p + n
    if t in 'fdilb':
        n, enc, clen = struct.unpack_from('<III', buf, p); p += 12
        raw = bytes(buf[p:p + clen]); p += clen
        if enc: raw = zlib.decompress(raw)
        fmt = {'f': 'f', 'd': 'd', 'i': 'i', 'l': 'q', 'b': 'B'}[t]
        return list(struct.unpack('<%d%s' % (n, fmt), raw)), p
    raise ValueError("bad prop type " + t)


def _node(buf, p, wide):
    if wide:
        end, nprops, plen, nlen = struct.unpack_from('<QQQB', buf, p); p += 25
    else:
        end, nprops, plen, nlen = struct.unpack_from('<IIIB', buf, p); p += 13
    if end == 0:
        return None, p
    n = Node(bytes(buf[p:p + nlen]).decode())
    p += nlen
    for _ in range(nprops):
        v, p = _prop(buf, p)
        n.props.append(v)
    while p < end:
        c, p = _node(buf, p, wide)
        if c is None: break
        n.children.append(c)
    return n, end


def load(path):
    buf = open(path, 'rb').read()
    ver = struct.unpack_from('<I', buf, 23)[0]
    wide = ver >= 7500
    p, root = 27, Node("")
    while p < len(buf) - 40:
        n, p = _node(buf, p, wide)
        if n is None: break
        root.children.append(n)
    return root, ver


if __name__ == "__main__":
    import sys
    root, ver = load(sys.argv[1])
    print("version", ver, [c.name for c in root.children])
    objs = root.first("Objects")
    from collections import Counter
    print(Counter(c.name + ":" + (c.props[2] if len(c.props) > 2 else "") for c in objs.children))
    for c in objs.children:
        if c.name == "Model" and c.props[2] in ("LimbNode", "Root"):
            print(c.props[1].split("\x00")[0])
