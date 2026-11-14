# OBJ parser benchmark

`parse_obj_bench.cc` generates deterministic triangle-soup models and reports
parse wall time, peak RSS (`ru_maxrss`), output counts, and an FNV-1a digest of
all parsed output arrays.

Build the feature-enabled configuration (MMAP + SIMD + multithreading):

```sh
g++ -std=c++11 -O3 -DNDEBUG -march=native \
  -DTINYOBJLOADER_USE_MULTITHREADING -DTINYOBJLOADER_USE_SIMD \
  -I. tests/bench/parse_obj_bench.cc -pthread -o parse_obj_bench
```

Generate and parse models:

```sh
./parse_obj_bench generate 250000 /tmp/tris-250k.obj
/usr/bin/time -v ./parse_obj_bench parse /tmp/tris-250k.obj 1
```

The optional third argument selects the worker count.
