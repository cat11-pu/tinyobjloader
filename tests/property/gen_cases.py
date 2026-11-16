#!/usr/bin/env python3
"""Deterministic OBJ test-case generator.

Generates:
  cases/valid/      50 well-formed OBJ files (for differential comparison)
  cases/malformed/  200 malformed OBJ files, categories alternating:
                    trunc -> negidx -> badface -> hugeverts
"""

import os
import random

CATEGORIES = ["trunc", "negidx", "badface", "hugeverts"]


def rand_float(rng):
    kind = rng.randrange(6)
    if kind == 0:
        return str(rng.randint(-100, 100))
    if kind == 1:
        return "%.6f" % rng.uniform(-50, 50)
    if kind == 2:
        return "%.17g" % rng.uniform(-1, 1)
    if kind == 3:
        return "%+.3e" % rng.uniform(-10, 10)
    if kind == 4:
        return ".%03d" % rng.randint(0, 999)
    return "-%d.%02d" % (rng.randint(0, 9), rng.randint(0, 99))


def gen_valid_text(rng):
    """Build one well-formed OBJ document (as list of lines)."""
    lines = ["# generated valid case"]
    nv = rng.randint(3, 40)
    nvt = rng.choice([0, 0, 4, 12])
    nvn = rng.choice([0, 0, 3, 9])
    use_colors = rng.random() < 0.4
    use_vtw = nvt > 0 and rng.random() < 0.3

    if rng.random() < 0.3:
        lines.append("mtllib nonexistent.mtl")
    if rng.random() < 0.5:
        lines.append("o obj%d" % rng.randint(0, 999))

    for _ in range(nv):
        parts = [rand_float(rng) for _ in range(3)]
        if use_colors:
            parts += ["%.4f" % rng.random() for _ in range(3)]
        lines.append("v " + " ".join(parts))
    for _ in range(nvt):
        parts = [rand_float(rng) for _ in range(2)]
        if use_vtw:
            parts.append(rand_float(rng))
        lines.append("vt " + " ".join(parts))
    for _ in range(nvn):
        lines.append("vn " + " ".join(rand_float(rng) for _ in range(3)))

    if rng.random() < 0.25:
        lines.append("t gentag 2/1/1 7 -3 0.5 strval")

    def idx(n, allow_rel=True):
        if allow_rel and rng.random() < 0.3:
            return str(-rng.randint(1, n))
        return str(rng.randint(1, n))

    def triple():
        v = idx(nv)
        form = rng.randrange(4)
        if form == 0 or (nvt == 0 and nvn == 0):
            return v
        if form == 1 and nvt > 0:
            return "%s/%s" % (v, idx(nvt))
        if form == 2 and nvn > 0:
            return "%s//%s" % (v, idx(nvn))
        if nvt > 0 and nvn > 0:
            return "%s/%s/%s" % (v, idx(nvt), idx(nvn))
        return v

    n_groups = rng.randint(1, 3)
    for g in range(n_groups):
        if rng.random() < 0.6:
            lines.append("g grp%d" % g)
        if rng.random() < 0.4:
            lines.append("s %d" % rng.randint(0, 4))
        if rng.random() < 0.3:
            lines.append("usemtl mat%d" % rng.randint(0, 5))
        for _ in range(rng.randint(1, 6)):
            k = rng.randint(3, 6)
            lines.append("f " + " ".join(triple() for _ in range(k)))
        if rng.random() < 0.3:
            lines.append("l " + " ".join(triple()
                                         for _ in range(rng.randint(2, 5))))
        if rng.random() < 0.3:
            lines.append("p " + " ".join(triple()
                                         for _ in range(rng.randint(1, 4))))
        if rng.random() < 0.2:
            lines.append("# trailing comment")
            lines.append("")

    text = "\n".join(lines) + "\n"
    if rng.random() < 0.25:
        text = text.replace("\n", "\r\n")
    if rng.random() < 0.2:
        text = text.replace(" ", "\t", 5)
    return text


# ---------------- malformed mutations ----------------

def mut_trunc(rng, text):
    mode = rng.randrange(3)
    if mode == 0:
        # cut at random byte offset (may split a token / CRLF)
        cut = rng.randint(1, max(1, len(text) - 1))
        return text[:cut]
    if mode == 1:
        # drop the last N lines
        lines = text.split("\n")
        n = rng.randint(1, max(1, len(lines) - 1))
        return "\n".join(lines[:-n])
    # keep only the first N bytes of the last line
    lines = text.split("\n")
    if lines:
        lines[-1] = lines[-1][:rng.randint(0, 3)]
    return "\n".join(lines)


def mut_negidx(rng, text):
    lines = text.split("\n")
    face_lines = [i for i, l in enumerate(lines) if l.startswith("f ")]
    nverts = sum(1 for l in lines if l.startswith("v "))
    if not face_lines:
        lines.append("f 1 2 3")
        face_lines = [len(lines) - 1]
    i = rng.choice(face_lines)
    toks = lines[i].split()
    j = rng.randrange(1, len(toks))
    choice = rng.randrange(5)
    if choice == 0:
        toks[j] = str(-rng.randint(1, max(1, nverts)))  # valid relative
    elif choice == 1:
        toks[j] = str(-(nverts + rng.randint(1, 1000)))  # out of range
    elif choice == 2:
        toks[j] = "-2147483648"  # INT_MIN
    elif choice == 3:
        toks[j] = "%d//%d" % (-rng.randint(1, 5), -rng.randint(1, 100))
    else:
        toks[j] = "-0"
    lines[i] = " ".join(toks)
    return "\n".join(lines)


def mut_badface(rng, text):
    lines = text.split("\n")
    bad = rng.choice([
        "f 0 0 0",
        "f 1 2 0",
        "f a b c",
        "f 1/2/3/4 5/6/7/8 9/10/11/12",
        "f",
        "f ",
        "f 1",
        "f 1 2",
        "f 99999999999999999999 2 3",
        "f 1.5 2.5 3.5",
        "f --3 2 1",
        "f 1,,2 2 3",
        "f 1/ 2/ 3/",
        "f // // //",
        "f /1/2 /3/4 /5/6",
        "f 1//x 2 3",
        "f 1 2 3\x00 4",
        "f 3 2 1 # comment\nf",
    ])
    pos = rng.randint(0, len(lines) - 1)
    lines.insert(pos, bad)
    return "\n".join(lines)


def mut_hugeverts(rng, text):
    lines = text.split("\n")
    mode = rng.randrange(4)
    if mode == 0:
        # extreme float magnitudes
        lines.insert(rng.randint(0, len(lines) - 1),
                     rng.choice([
                         "v 1e308 1e308 1e308",
                         "v -1e308 1e-308 0",
                         "v 1e999 0 0",
                         "v 3.4028234663852886e+39 0 0",
                         "v 0.0000000000000000000000001 0 0",
                     ]))
    elif mode == 1:
        # indices near / beyond INT_MAX
        idx = rng.choice(["2147483647", "2147483648", "4294967295",
                          "9999999999", "-2147483647"])
        lines.append("f %s %s %s" % (idx, idx, idx))
    elif mode == 2:
        # genuinely large vertex section (memory stress, ~100k verts)
        big = ["v %d.%d %d %d" % (i, i % 7, i % 3, i % 5)
               for i in range(rng.randint(20000, 100000))]
        lines[1:1] = big
        lines.append("f 1 50000 99999")
    else:
        # absurd counts in one face
        lines.append("f " + " ".join(str(rng.randint(1, 10**9))
                                     for _ in range(rng.randint(50, 300))))
    return "\n".join(lines)


MUTATORS = {"trunc": mut_trunc, "negidx": mut_negidx,
            "badface": mut_badface, "hugeverts": mut_hugeverts}


def generate(outdir, n_malformed=200, n_valid=50):
    valid_dir = os.path.join(outdir, "valid")
    mal_dir = os.path.join(outdir, "malformed")
    os.makedirs(valid_dir, exist_ok=True)
    os.makedirs(mal_dir, exist_ok=True)

    rng = random.Random(20261117)
    valid_texts = []
    for i in range(n_valid):
        text = gen_valid_text(rng)
        valid_texts.append(text)
        with open(os.path.join(valid_dir, "valid_%03d.obj" % i), "w") as f:
            f.write(text)

    rng = random.Random(7)
    base_rng = random.Random(424242)
    for i in range(n_malformed):
        cat = CATEGORIES[i % len(CATEGORIES)]
        base = gen_valid_text(base_rng)
        text = MUTATORS[cat](rng, base)
        name = "mal_%03d_%s.obj" % (i, cat)
        with open(os.path.join(mal_dir, name), "w",
                  errors="surrogateescape") as f:
            f.write(text)
    return valid_dir, mal_dir


if __name__ == "__main__":
    import sys
    outdir = sys.argv[1] if len(sys.argv) > 1 else "cases"
    v, m = generate(outdir)
    print("valid:     %s (%d files)" % (v, len(os.listdir(v))))
    print("malformed: %s (%d files)" % (m, len(os.listdir(m))))
