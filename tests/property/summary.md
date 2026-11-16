# tinyobjloader property test summary

Binary: `tests/property/build/obj_dump` (ASan+UBSan+LSan, `-DTINYOBJLOADER_USE_DOUBLE`)

## Phase 1: malformed corpus (200 files)

| category | parsed | rejected (line-numbered error) | crash/sanitizer |
|---|---|---|---|
| trunc | 49 | 1 | 0 |
| negidx | 11 | 39 | 0 |
| badface | 23 | 27 | 0 |
| hugeverts | 41 | 9 | 0 |

Crashes / sanitizer reports / leaks: **0**

Rejected files all carry parser line numbers; sample:

- `mal_006_badface.obj`: rejected at input line(s) [5]
- `mal_010_badface.obj`: rejected at input line(s) [19]
- `mal_013_negidx.obj`: rejected at input line(s) [37]
- `mal_014_badface.obj`: rejected at input line(s) [36]
- `mal_017_negidx.obj`: rejected at input line(s) [38]
- `mal_018_badface.obj`: rejected at input line(s) [31]
- `mal_019_hugeverts.obj`: rejected at input line(s) [4]
- `mal_021_negidx.obj`: rejected at input line(s) [31]
- `mal_022_badface.obj`: rejected at input line(s) [30]
- `mal_025_negidx.obj`: rejected at input line(s) [56]

## Phase 2: differential vs reference (50 files)

Identical dumps: **50**, mismatches: **0**

Counterexample artifacts: `tests/property/counterexamples` (0 files)
