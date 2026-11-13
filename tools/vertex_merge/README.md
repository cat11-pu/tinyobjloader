# Parse-stage vertex welding

OBJ files commonly repeat the full position/normal/texcoord record for
every face corner, so GPU memory is spent on many identical vertices.
This tool welds corners whose position, normal and texture coordinate are
exactly equal into one vertex and rebuilds the index buffer from a remap
table.

## Files

- `weld_vertices.h` — the weld pass itself: FNV-1a hashing over the raw
  vertex bytes plus an open-addressing hash table kept near 50% load.
  Equality uses exact float comparison, so only bit-identical vertices
  merge and the rendered result cannot change. Expected O(n) time.
- `rasterizer.h` — deterministic software rasterizer (perspective-correct
  interpolation, fixed tie rules) used to render the mesh before and
  after welding and prove pixel-identical output.
- `vertex_merge_tool.cc` — driver: parses with tinyobjloader, fans
  polygons into triangles, expands corners, welds them, renders both
  variants and writes stats plus PPM images.
- `generate_wavy_grid.py` — regenerates a duplicated-corner test grid
  (one `v`/`vt`/`vn` triple per corner).
- `make_comparison.py` — assembles `before.ppm` and `after.ppm` into a
  labeled `render_compare.png`.

## Build and run

```sh
g++ -std=c++17 -O2 -I. \
    tools/vertex_merge/vertex_merge_tool.cc -o vertex_merge_tool
./vertex_merge_tool artifacts/vertex_merge/wavy_grid.obj \
    artifacts/vertex_merge 1024 768
python3 tools/vertex_merge/make_comparison.py artifacts/vertex_merge
```

## Outputs

- `stats.json` / `stats.txt` — vertex and index counts, pixel diffs,
  parse and weld timings.
- `before.ppm`, `after.ppm` — raw renders (byte-identical on success).
- `render_compare.png` — side-by-side comparison with the stats overlay.

The tool exits non-zero if any rendered pixel differs or the rebuilt
index table does not resolve back to the original corner data.

## Correctness argument

Each output index points at a vertex whose eight floats are exactly equal
to the original corner's attributes, so every triangle feeds the
rasterizer the same per-vertex values in the same order. Triangle
connectivity is unchanged: one remap entry is emitted per input corner.
The driver additionally checks every remap entry against its source
corner and compares both renders pixel by pixel.
