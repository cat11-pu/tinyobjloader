#!/usr/bin/env python3
"""Reference OBJ parser for differential testing against tinyobjloader.

Replicates tinyobjloader's LoadObj semantics (triangulate=False,
vertex_color=True, empty MTL) and emits the same canonical dump format as
obj_dump.cc. Exits 3 with a line-numbered message on parse errors.
"""

import re
import sys


class ParseError(Exception):
    def __init__(self, line_no, msg):
        super().__init__(msg)
        self.line_no = line_no
        self.msg = msg


_INT_PREFIX = re.compile(r"^[ \t]*([+-]?\d+)")


def c_atoi(s):
    """atoi semantics: leading integer prefix, 0 if none."""
    m = _INT_PREFIX.match(s)
    return int(m.group(1)) if m else 0


def fix_index(idx, n, allow_zero):
    """Returns (value, ok). Mirrors tinyobjloader's fixIndex."""
    if idx > 0:
        return idx - 1, True
    if idx == 0:
        return -1, allow_zero
    ret = n + idx
    if ret < 0:
        return ret, False
    return ret, True


def parse_triple(tok, nv, nvn, nvt, line_no):
    """Parse one face/line/point token into (v, vt, vn)."""
    parts = tok.split("/")
    v, ok = fix_index(c_atoi(parts[0]), nv, False)
    if not ok:
        raise ParseError(line_no, "invalid vertex index")
    vt = -1
    vn = -1
    if len(parts) >= 2:
        if parts[1] == "" and len(parts) == 2:
            # "i/" : atoi("") == 0, vt allows zero -> -1
            vt = -1
        elif parts[1] != "":
            vt, ok = fix_index(c_atoi(parts[1]), nvt, True)
            if not ok:
                raise ParseError(line_no, "invalid texcoord index")
    if len(parts) >= 3:
        vn, ok = fix_index(c_atoi(parts[2]), nvn, True)
        if not ok:
            raise ParseError(line_no, "invalid normal index")
    return (v, vt, vn)


def fmt(x):
    return format(x, ".17g")


def parse(text):
    """Parse OBJ text. Returns dump string. Raises ParseError."""
    vertices = []   # flat xyz
    vcolors = []    # flat rgb (only from 6-component v lines)
    weights = []    # per-vertex weight (4th component or 1.0)
    normals = []    # flat xyz
    texcoords = []  # flat uv
    texcoord_ws = []
    tags = []       # global, attached to every flushed shape (with faces)

    shapes = []     # finalized shapes
    shape = None    # accumulating shape dict

    prim_faces = []   # list of (smoothing_id, [triples])
    prim_lines = []   # list of [triples]
    prim_points = []  # list of [triples]

    name = ""
    material = -1
    smoothing = 0

    def new_shape():
        return {"name": "", "faces": [], "lines": [], "points": [],
                "tags": []}

    shape = new_shape()

    def flush():
        """exportGroupsToShape: merge prim groups into current shape."""
        nonlocal shape
        if not prim_faces and not prim_lines and not prim_points:
            return False
        shape["name"] = name
        if prim_faces:
            for sm, triples in prim_faces:
                if len(triples) < 3:
                    continue  # degenerated face
                shape["faces"].append((material, sm, triples))
            shape["tags"] = list(tags)
        for triples in prim_lines:
            shape["lines"].append(list(triples))
        for triples in prim_points:
            shape["points"].extend(triples)
        return True

    def clear_prims():
        del prim_faces[:]
        del prim_lines[:]
        del prim_points[:]

    lines = text.split("\n")
    for line_no, raw in enumerate(lines, start=1):
        line = raw.rstrip("\r")
        s = line.lstrip(" \t")
        if not s or s.startswith("#"):
            continue

        # strip trailing comment for token-based commands
        body = s.split("#", 1)[0]
        toks = body.split()

        cmd = s[0]
        second = s[1] if len(s) > 1 else ""

        if cmd == "v" and second in " \t":
            reals = []
            for t in toks[1:7]:
                try:
                    reals.append(float(t))
                except ValueError:
                    break
            if len(reals) < 3:
                raise ParseError(line_no, "failed to parse `v' line")
            vertices.extend(reals[0:3])
            # ObjReader uses default_vcols_fallback=true: colors are always
            # emitted, (1,1,1) when absent.
            if len(reals) >= 6:
                weights.append(reals[3])
                vcolors.extend(reals[3:6])
            elif len(reals) == 4:
                weights.append(reals[3])
                vcolors.extend([reals[3], 1.0, 1.0])
            else:
                # 3 comps, or 5 comps (quirk: colors reset, weight 1.0)
                weights.append(1.0)
                vcolors.extend([1.0, 1.0, 1.0])
            continue

        if cmd == "v" and second == "n" and len(s) > 2 and s[2] in " \t":
            reals = []
            for t in toks[1:4]:
                try:
                    reals.append(float(t))
                except ValueError:
                    break
            if len(reals) < 3:
                raise ParseError(line_no, "failed to parse `vn' line")
            normals.extend(reals[0:3])
            continue

        if cmd == "v" and second == "t" and len(s) > 2 and s[2] in " \t":
            reals = []
            for t in toks[1:4]:
                try:
                    reals.append(float(t))
                except ValueError:
                    break
            if len(reals) < 2:
                raise ParseError(line_no, "failed to parse `vt' line")
            texcoords.extend(reals[0:2])
            texcoord_ws.append(reals[2] if len(reals) >= 3 else 0.0)
            continue

        if cmd == "f" and second in " \t":
            nv, nvn, nvt = (len(vertices) // 3, len(normals) // 3,
                            len(texcoords) // 2)
            triples = [parse_triple(t, nv, nvn, nvt, line_no)
                       for t in toks[1:]]
            prim_faces.append((smoothing, triples))
            continue

        if cmd == "l" and second in " \t":
            nv, nvn, nvt = (len(vertices) // 3, len(normals) // 3,
                            len(texcoords) // 2)
            triples = [parse_triple(t, nv, nvn, nvt, line_no)
                       for t in toks[1:]]
            prim_lines.append(triples)
            continue

        if cmd == "p" and second in " \t":
            nv, nvn, nvt = (len(vertices) // 3, len(normals) // 3,
                            len(texcoords) // 2)
            triples = [parse_triple(t, nv, nvn, nvt, line_no)
                       for t in toks[1:]]
            prim_points.append(triples)
            continue

        if cmd == "g" and second in " \t":
            flush()
            if shape["faces"]:
                shapes.append(shape)
            shape = new_shape()
            clear_prims()
            name = " ".join(toks[1:]) if len(toks) > 1 else ""
            continue

        if cmd == "o" and second in " \t":
            flush()
            if shape["faces"] or shape["lines"] or shape["points"]:
                shapes.append(shape)
            shape = new_shape()
            clear_prims()
            name = s[2:].rstrip("\r")
            continue

        if cmd == "s" and second in " \t":
            rest = toks[1:] 
            if not rest:
                continue
            if rest[0] == "off":
                smoothing = 0
            else:
                sid = c_atoi(rest[0])
                smoothing = sid if sid >= 0 else 0
            continue

        if s.startswith("usemtl") and len(s) > 6 and s[6] in " \t":
            # empty MTL: material stays -1, no flush (id unchanged)
            continue

        if s.startswith("mtllib") and len(s) > 6 and s[6] in " \t":
            continue

        if cmd == "t" and second in " \t":
            # t name i/r/s ints... reals... strings...
            tname = toks[1] if len(toks) > 1 else ""
            trip = toks[2].split("/") if len(toks) > 2 else []
            sizes = [c_atoi(x) for x in trip]
            while len(sizes) < 3:
                sizes.append(0)
            ni, nr, ns_ = [max(0, min(8192, x)) for x in sizes]
            rest = toks[3:]
            ints = [c_atoi(x) for x in rest[:ni]]
            reals = []
            for x in rest[ni:ni + nr]:
                try:
                    reals.append(float(x))
                except ValueError:
                    reals.append(0.0)
            strs = rest[ni + nr:ni + nr + ns_]
            tags.append((tname, ints, reals, strs))
            continue

        # unknown command: ignored

    ret = flush()
    if ret or shape["faces"]:
        shapes.append(shape)

    out = ["OK"]
    out.append("V %d" % (len(vertices) // 3))
    for i in range(0, len(vertices), 3):
        out.append("%s %s %s" % (fmt(vertices[i]), fmt(vertices[i + 1]),
                                 fmt(vertices[i + 2])))
    out.append("W %d" % len(weights))
    out.extend(fmt(w) for w in weights)
    out.append("VC %d" % (len(vcolors) // 3))
    for i in range(0, len(vcolors), 3):
        out.append("%s %s %s" % (fmt(vcolors[i]), fmt(vcolors[i + 1]),
                                 fmt(vcolors[i + 2])))
    out.append("VT %d" % (len(texcoords) // 2))
    for i in range(0, len(texcoords), 2):
        out.append("%s %s" % (fmt(texcoords[i]), fmt(texcoords[i + 1])))
    out.append("VTW %d" % len(texcoord_ws))
    out.extend(fmt(w) for w in texcoord_ws)
    out.append("VN %d" % (len(normals) // 3))
    for i in range(0, len(normals), 3):
        out.append("%s %s %s" % (fmt(normals[i]), fmt(normals[i + 1]),
                                 fmt(normals[i + 2])))
    out.append("SHAPES %d" % len(shapes))
    for sh in shapes:
        out.append("SHAPE %s" % sh["name"])
        out.append("FACES %d" % len(sh["faces"]))
        for mat, sm, triples in sh["faces"]:
            idx = " ".join("%d/%d/%d" % t for t in triples)
            out.append("F %d %d %d %s" % (len(triples), mat, sm, idx))
        out.append("LINES %d" % len(sh["lines"]))
        for triples in sh["lines"]:
            idx = " ".join("%d/%d/%d" % t for t in triples)
            out.append("L %d%s%s" % (len(triples), " " if idx else "", idx))
        out.append("POINTS %d" % len(sh["points"]))
        for t in sh["points"]:
            out.append("P %d/%d/%d" % t)
        out.append("TAGS %d" % len(sh["tags"]))
        for tname, ints, reals, strs in sh["tags"]:
            line = "T %s I" % tname
            line += "".join(" %d" % i for i in ints)
            line += " F" + "".join(" " + fmt(r) for r in reals)
            line += " S" + "".join(" " + x for x in strs)
            out.append(line)
    out.append("END")
    return "\n".join(out) + "\n"


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: ref_parse.py <file.obj>\n")
        return 2
    with open(sys.argv[1], "rb") as f:
        text = f.read().decode("latin-1")
    try:
        dump = parse(text)
    except ParseError as e:
        sys.stderr.write("%s:%d: error: %s\n" % (sys.argv[1], e.line_no, e.msg))
        return 3
    sys.stdout.write(dump)
    return 0


if __name__ == "__main__":
    sys.exit(main())
