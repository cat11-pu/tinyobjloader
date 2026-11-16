/*
 * tester_c.c -- loader tests for tiny_obj_c (pure C11, acutest).
 * Run from the tests/ directory so that "../models/..." paths resolve.
 * Built with TOBJ_ENABLE_FILE_IO + TOBJ_ENABLE_MMAP under ASan+UBSan.
 */
#include "acutest.h"
#include "tiny_obj_c.h"

#include <stdlib.h>
#include <string.h>

/* ---- helpers ----------------------------------------------------------- */

static tobj_result load_file_f(const char *path, tobj_scene_f *sc) {
  tobj_load_config cfg = tobj_default_config();
  return tobj_load_obj_from_file_f(sc, path, &cfg, NULL);
}

/* sum of per-face vertex counts must equal the index count */
static int mesh_consistent_f(const tobj_scene_f *sc) {
  for (size_t s = 0; s < sc->num_shapes; s++) {
    const tobj_mesh_f *m = &sc->shapes[s].mesh;
    size_t sum = 0;
    for (size_t f = 0; f < m->num_faces; f++) sum += m->num_face_vertices[f];
    if (sum != m->num_indices) return 0;
    /* every index that references a vertex must be in range */
    size_t numv = sc->attrib.vertices.count / 3;
    for (size_t i = 0; i < m->num_indices; i++) {
      int vi = m->indices[i].vertex_index;
      if (vi >= 0 && (size_t)vi >= numv) return 0;
    }
  }
  return 1;
}

static const char *load_string_f(const char *obj, tobj_scene_f *sc,
                                  bool triangulate) {
  tobj_load_config cfg = tobj_default_config();
  cfg.triangulate = triangulate;
  tobj_result r =
      tobj_load_obj_from_memory_f(sc, (const uint8_t *)obj, strlen(obj), &cfg,
                                  NULL);
  return tobj_result_string(r);
}

typedef struct mem_io {
  const uint8_t *data;
  size_t len;
  size_t pos;
  size_t chunk;
  int closed;
} mem_io;

static tobj_result mem_io_read(void *ud, uint8_t *dst, size_t dst_size,
                               size_t *bytes_read) {
  mem_io *m = (mem_io *)ud;
  size_t left = m->len - m->pos;
  size_t n = left < dst_size ? left : dst_size;
  if (m->chunk && n > m->chunk) n = m->chunk;
  if (n) memcpy(dst, m->data + m->pos, n);
  m->pos += n;
  *bytes_read = n;
  return TOBJ_OK;
}

static void mem_io_close(void *ud) { ((mem_io *)ud)->closed = 1; }

static tobj_result io_overreport_read(void *ud, uint8_t *dst, size_t dst_size,
                                      size_t *bytes_read) {
  (void)ud;
  if (dst_size) dst[0] = 'x';
  *bytes_read = dst_size + 1;
  return TOBJ_OK;
}

static tobj_result io_error_read(void *ud, uint8_t *dst, size_t dst_size,
                                 size_t *bytes_read) {
  (void)ud;
  (void)dst;
  (void)dst_size;
  *bytes_read = 0;
  return TOBJ_ERR_IO;
}

typedef struct test_mtl_entry {
  const char *name;
  const char *data;
} test_mtl_entry;

typedef struct test_mtl_set {
  const test_mtl_entry *entries;
  size_t count;
} test_mtl_set;

static tobj_result test_mtl_resolver(void *ud, const char *name,
                                     tobj_mtl_source *out) {
  const test_mtl_set *set = (const test_mtl_set *)ud;
  for (size_t i = 0; i < set->count; i++) {
    if (strcmp(set->entries[i].name, name) == 0) {
      out->data = (const uint8_t *)set->entries[i].data;
      out->len = strlen(set->entries[i].data);
      out->release = NULL;
      out->release_ud = NULL;
      return TOBJ_OK;
    }
  }
  return TOBJ_ERR_NOT_FOUND;
}

typedef struct alloc_stats {
  size_t allocs;
  size_t frees;
  size_t live;
  size_t bytes;
} alloc_stats;

static void *stats_alloc(void *ud, size_t size, size_t align) {
  alloc_stats *s = (alloc_stats *)ud;
  (void)align;
  void *p = malloc(size ? size : 1);
  if (p) {
    s->allocs++;
    s->live++;
    s->bytes += size;
  }
  return p;
}

static void stats_free(void *ud, void *ptr, size_t size) {
  alloc_stats *s = (alloc_stats *)ud;
  (void)size;
  if (!ptr) return;
  s->frees++;
  s->live--;
  free(ptr);
}

static tobj_allocator stats_allocator(alloc_stats *stats) {
  tobj_allocator a = tobj_default_allocator();
  a.alloc = stats_alloc;
  a.calloc = NULL;
  a.realloc = NULL;
  a.free = stats_free;
  a.user_data = stats;
  return a;
}

static uint64_t digest_bytes(uint64_t h, const void *data, size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint64_t)p[i];
    h *= UINT64_C(1099511628211);
  }
  return h;
}

static uint64_t digest_u64(uint64_t h, uint64_t v) {
  return digest_bytes(h, &v, sizeof v);
}

static uint64_t digest_string(uint64_t h, const char *s) {
  size_t len = s ? strlen(s) : 0;
  h = digest_u64(h, (uint64_t)len);
  return len ? digest_bytes(h, s, len) : h;
}

static uint64_t digest_real(uint64_t h, float v) {
  uint32_t bits;
  memcpy(&bits, &v, sizeof bits);
  return digest_u64(h, bits);
}

static uint64_t material_map_digest(const tobj_material_map_f *map) {
  uint64_t h = UINT64_C(1469598103934665603);
  h = digest_u64(h, UINT64_C(0x4d41504d41503031)); /* "MAPMAP01" */
  h = digest_u64(h, map->num_materials);
  h = digest_u64(h, map->num_source_materials);
  h = digest_u64(h, map->num_face_material_ids);
  h = digest_u64(h, map->num_shapes);
  h = digest_u64(h, map->num_texture_slots);
  h = digest_u64(h, map->num_textures);

  for (size_t i = 0; i < map->num_materials; i++) {
    const tobj_material_f *m = &map->materials[i];
    h = digest_string(h, m->name);
    for (int k = 0; k < 3; k++) {
      h = digest_real(h, m->ambient[k]);
      h = digest_real(h, m->diffuse[k]);
      h = digest_real(h, m->specular[k]);
    }
    h = digest_real(h, m->shininess);
    h = digest_real(h, m->dissolve);
  }
  for (size_t i = 0; i < map->num_source_materials; i++)
    h = digest_u64(h, (uint64_t)(int64_t)map->source_to_material[i]);
  for (size_t i = 0; i < map->num_shapes + 1; i++)
    h = digest_u64(h, map->shape_face_offsets[i]);
  for (size_t i = 0; i < map->num_face_material_ids; i++)
    h = digest_u64(h, (uint64_t)(int64_t)map->face_material_ids[i]);
  for (size_t i = 0; i < map->num_textures; i++) {
    h = digest_string(h, map->textures[i].path);
    h = digest_u64(h, map->textures[i].num_references);
  }
  for (size_t i = 0; i < map->num_texture_slots; i++) {
    const tobj_texture_slot_f *slot = &map->texture_slots[i];
    h = digest_u64(h, (uint64_t)slot->kind);
    h = digest_u64(h, (uint64_t)(int64_t)slot->material_id);
    h = digest_u64(h, (uint64_t)(int64_t)slot->texture_id);
    h = digest_string(h, slot->path);
    h = digest_real(h, slot->option->scale[0]);
  }
  return h;
}

/* ---- corpus ------------------------------------------------------------ */

/* Only files committed to the repo (CI checks out a clean tree; sandbox files
 * are local scratch files and are intentionally not used here). The
 * pathological-geometry triangulation cases are covered directly by
 * tests/tess_tester.c. */
static const char *kCorpus[] = {
    "../models/cube.obj",
    "../models/cornell_box.obj",
    "../models/catmark_torus_creases0.obj",
    "../models/smoothing-group-two-squares.obj",
    "../models/issue-162-smoothing-group.obj",
    "../models/smoothing-normal.obj",
    "../models/issue-295-trianguation-failure.obj",
    NULL};

static void test_corpus_loads(void) {
  for (int i = 0; kCorpus[i]; i++) {
    tobj_scene_f sc;
    tobj_result r = load_file_f(kCorpus[i], &sc);
    TEST_CHECK_(r == TOBJ_OK, "%s -> %s", kCorpus[i], tobj_result_string(r));
    if (r == TOBJ_OK) {
      TEST_CHECK_(mesh_consistent_f(&sc), "%s mesh consistent", kCorpus[i]);
      tobj_scene_free_f(&sc);
    }
  }
}

static void test_both_precisions(void) {
  tobj_scene_f sf;
  tobj_scene_d sd;
  tobj_load_config cfg = tobj_default_config();
  TEST_CHECK(tobj_load_obj_from_file_f(&sf, "../models/cube.obj", &cfg, NULL) ==
             TOBJ_OK);
  TEST_CHECK(tobj_load_obj_from_file_d(&sd, "../models/cube.obj", &cfg, NULL) ==
             TOBJ_OK);
  /* identical topology across precision families */
  TEST_CHECK(sf.num_shapes == sd.num_shapes);
  TEST_CHECK(sf.attrib.vertices.count == sd.attrib.vertices.count);
  size_t fi = 0, di = 0;
  for (size_t s = 0; s < sf.num_shapes; s++) fi += sf.shapes[s].mesh.num_indices;
  for (size_t s = 0; s < sd.num_shapes; s++) di += sd.shapes[s].mesh.num_indices;
  TEST_CHECK(fi == di);
  tobj_scene_free_f(&sf);
  tobj_scene_free_d(&sd);
}

/* ---- targeted regressions --------------------------------------------- */

static void test_quad_triangulation(void) {
  tobj_scene_f sc;
  const char *obj = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n";
  load_string_f(obj, &sc, true);
  TEST_CHECK(sc.num_shapes == 1);
  TEST_CHECK(sc.shapes[0].mesh.num_faces == 2);   /* 2 triangles */
  TEST_CHECK(sc.shapes[0].mesh.num_indices == 6); /* 6 indices */
  tobj_scene_free_f(&sc);
}

static void test_no_triangulation(void) {
  tobj_scene_f sc;
  const char *obj = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n";
  load_string_f(obj, &sc, false);
  TEST_CHECK(sc.shapes[0].mesh.num_faces == 1);
  TEST_CHECK(sc.shapes[0].mesh.num_face_vertices[0] == 4);
  TEST_CHECK(sc.shapes[0].mesh.num_indices == 4);
  tobj_scene_free_f(&sc);
}

static void test_negative_relative_index(void) {
  tobj_scene_f sc;
  /* -1 -2 -3 refer to the three most recent vertices */
  const char *obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf -3 -2 -1\n";
  load_string_f(obj, &sc, true);
  TEST_CHECK(sc.shapes[0].mesh.num_indices == 3);
  TEST_CHECK(sc.shapes[0].mesh.indices[0].vertex_index == 0);
  TEST_CHECK(sc.shapes[0].mesh.indices[1].vertex_index == 1);
  TEST_CHECK(sc.shapes[0].mesh.indices[2].vertex_index == 2);
  tobj_scene_free_f(&sc);
}

static void test_bom_and_crlf(void) {
  tobj_scene_f sc;
  const char obj[] = "\xEF\xBB\xBFv 0 0 0\r\nv 1 0 0\r\nv 0 1 0\r\nf 1 2 3\r\n";
  tobj_load_config cfg = tobj_default_config();
  tobj_result r = tobj_load_obj_from_memory_f(&sc, (const uint8_t *)obj,
                                              sizeof(obj) - 1, &cfg, NULL);
  TEST_CHECK(r == TOBJ_OK);
  TEST_CHECK(sc.attrib.vertices.count == 9);
  TEST_CHECK(sc.shapes[0].mesh.num_indices == 3);
  tobj_scene_free_f(&sc);
}

static void test_vertex_colors(void) {
  tobj_scene_f sc;
  const char *obj = "v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\nf 1 2 3\n";
  load_string_f(obj, &sc, true);
  TEST_CHECK(sc.attrib.colors.count == 9);
  TEST_CHECK(sc.attrib.colors.ptr[0] == 1.0f); /* red on v0 */
  TEST_CHECK(sc.attrib.colors.ptr[4] == 1.0f); /* green on v1 */
  tobj_scene_free_f(&sc);
}

static void test_color_fallback_white(void) {
  tobj_scene_f sc;
  const char *obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
  load_string_f(obj, &sc, true);
  TEST_CHECK(sc.attrib.colors.count == 9);
  for (int i = 0; i < 9; i++) TEST_CHECK(sc.attrib.colors.ptr[i] == 1.0f);
  tobj_scene_free_f(&sc);
}

static void test_lines_and_points(void) {
  tobj_scene_f sc;
  const char *obj =
      "v 0 0 0\nv 1 0 0\nv 2 0 0\nv 3 0 0\nl 1 2 3\np 1 4\n";
  load_string_f(obj, &sc, true);
  TEST_CHECK(sc.shapes[0].lines.num_lines == 1);
  TEST_CHECK(sc.shapes[0].lines.num_indices == 3);
  TEST_CHECK(sc.shapes[0].lines.num_line_vertices[0] == 3);
  TEST_CHECK(sc.shapes[0].points.num_indices == 2);
  tobj_scene_free_f(&sc);
}

static void test_degenerate_face_skipped(void) {
  tobj_scene_f sc;
  const char *obj = "v 0 0 0\nv 1 0 0\nf 1 2\nf 1 2 1\n"; /* <3 verts skipped */
  load_string_f(obj, &sc, true);
  /* the 2-vertex face is skipped; the 3-"vertex" one is kept */
  TEST_CHECK(sc.num_shapes == 1);
  tobj_scene_free_f(&sc);
}

static void test_empty_and_garbage(void) {
  tobj_scene_f sc;
  tobj_load_config cfg = tobj_default_config();
  TEST_CHECK(tobj_load_obj_from_memory_f(&sc, (const uint8_t *)"", 0, &cfg,
                                         NULL) == TOBJ_OK);
  tobj_scene_free_f(&sc);
  const char *garbage = "\x00\x01\x02 random !!! f f f\n v v v\n 99999999999";
  TEST_CHECK(tobj_load_obj_from_memory_f(&sc, (const uint8_t *)garbage, 40,
                                         &cfg, NULL) == TOBJ_OK);
  tobj_scene_free_f(&sc);
}

static void test_mtl_standalone(void) {
  tobj_material_list_f ml;
  const char *mtl =
      "newmtl m\nKd 0.1 0.2 0.3\nNs 42\nmap_Kd -s 2 2 1 tex.png\nfoo bar baz\n";
  tobj_result r =
      tobj_parse_mtl_from_memory_f(&ml, (const uint8_t *)mtl, strlen(mtl), NULL,
                                   NULL);
  TEST_CHECK(r == TOBJ_OK);
  TEST_CHECK(ml.count == 1);
  TEST_CHECK(ml.items[0].diffuse[0] == 0.1f);
  TEST_CHECK(ml.items[0].shininess == 42.0f);
  TEST_CHECK(strcmp(ml.items[0].diffuse_texname, "tex.png") == 0);
  TEST_CHECK(ml.items[0].diffuse_texopt.scale[0] == 2.0f);
  const char *foo = tobj_material_get_param_f(&ml.items[0], "foo");
  TEST_CHECK(foo && strcmp(foo, "bar baz") == 0);
  tobj_material_list_free_f(&ml);
}

static void test_io_callbacks(void) {
  const char *obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
  mem_io mio;
  mio.data = (const uint8_t *)obj;
  mio.len = strlen(obj);
  mio.pos = 0;
  mio.chunk = 5;
  mio.closed = 0;
  tobj_io_callbacks io;
  io.read = mem_io_read;
  io.close = mem_io_close;
  io.user_data = &mio;
  tobj_load_config cfg = tobj_default_config();
  tobj_scene_f sc;
  tobj_result r = tobj_load_obj_from_io_f(&sc, &io, &cfg, NULL);
  TEST_CHECK(r == TOBJ_OK);
  TEST_CHECK(mio.closed == 1);
  TEST_CHECK(sc.num_shapes == 1);
  TEST_CHECK(sc.shapes[0].mesh.num_indices == 3);
  tobj_scene_free_f(&sc);
}

static void test_allocator_limit_failure(void) {
  const char *obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
  tobj_load_config cfg = tobj_default_config();
  cfg.allocator = tobj_default_allocator();
  cfg.allocator.max_alloc_size = 16;
  tobj_scene_f sc;
  tobj_result r =
      tobj_load_obj_from_memory_f(&sc, (const uint8_t *)obj, strlen(obj), &cfg,
                                  NULL);
  TEST_CHECK(r == TOBJ_ERR_ALLOC);
  TEST_CHECK(sc.num_shapes == 0);
}

static void test_input_limit_failure(void) {
  const char *obj = "v 0 0 0\n";
  tobj_load_config cfg = tobj_default_config();
  cfg.max_input_bytes = 4;
  tobj_scene_f sc;
  tobj_result r =
      tobj_load_obj_from_memory_f(&sc, (const uint8_t *)obj, strlen(obj), &cfg,
                                  NULL);
  TEST_CHECK(r == TOBJ_ERR_LIMIT_EXCEEDED);
}

static void test_io_input_limit_exact_and_over(void) {
  const char *obj = "v 0 0 0\n";
  tobj_load_config cfg = tobj_default_config();
  cfg.max_input_bytes = strlen(obj);

  mem_io exact;
  exact.data = (const uint8_t *)obj;
  exact.len = strlen(obj);
  exact.pos = 0;
  exact.chunk = 3;
  exact.closed = 0;
  tobj_io_callbacks io;
  io.read = mem_io_read;
  io.close = mem_io_close;
  io.user_data = &exact;
  tobj_scene_f sc;
  tobj_result r = tobj_load_obj_from_io_f(&sc, &io, &cfg, NULL);
  TEST_CHECK(r == TOBJ_OK);
  TEST_CHECK(exact.closed == 1);
  tobj_scene_free_f(&sc);

  const char *too_big = "v 0 0 0\n# extra\n";
  mem_io over;
  over.data = (const uint8_t *)too_big;
  over.len = strlen(too_big);
  over.pos = 0;
  over.chunk = 0;
  over.closed = 0;
  io.user_data = &over;
  r = tobj_load_obj_from_io_f(&sc, &io, &cfg, NULL);
  TEST_CHECK(r == TOBJ_ERR_LIMIT_EXCEEDED);
  TEST_CHECK(over.closed == 1);
  TEST_CHECK(sc.num_shapes == 0);
}

static void test_io_callback_errors(void) {
  tobj_io_callbacks io;
  io.close = mem_io_close;

  mem_io state;
  state.data = NULL;
  state.len = 0;
  state.pos = 0;
  state.chunk = 0;
  state.closed = 0;
  io.user_data = &state;
  io.read = io_overreport_read;
  tobj_scene_f sc;
  tobj_result r = tobj_load_obj_from_io_f(&sc, &io, NULL, NULL);
  TEST_CHECK(r == TOBJ_ERR_IO);
  TEST_CHECK(state.closed == 1);

  state.closed = 0;
  io.read = io_error_read;
  r = tobj_load_obj_from_io_f(&sc, &io, NULL, NULL);
  TEST_CHECK(r == TOBJ_ERR_IO);
  TEST_CHECK(state.closed == 1);
}

static void test_invalid_allocator_rejected(void) {
  const char *obj = "v 0 0 0\n";
  tobj_load_config cfg = tobj_default_config();
  cfg.allocator = tobj_default_allocator();
  cfg.allocator.free = NULL;
  tobj_scene_f sc;
  tobj_result r =
      tobj_load_obj_from_memory_f(&sc, (const uint8_t *)obj, strlen(obj), &cfg,
                                  NULL);
  TEST_CHECK(r == TOBJ_ERR_INVALID_ARG);
  TEST_CHECK(sc.num_shapes == 0);
}

static void test_allocator_without_calloc_or_realloc(void) {
  const char *obj =
      "v 0 0 0\nv 1 0 0\nv 2 0 0\nv 3 0 0\nv 4 0 0\n"
      "v 5 0 0\nv 6 0 0\nv 7 0 0\nv 8 0 0\n"
      "f 1 2 3\nf 4 5 6\nf 7 8 9\n";
  tobj_load_config cfg = tobj_default_config();
  cfg.allocator = tobj_default_allocator();
  cfg.allocator.calloc = NULL;
  cfg.allocator.realloc = NULL;
  tobj_scene_f sc;
  tobj_result r =
      tobj_load_obj_from_memory_f(&sc, (const uint8_t *)obj, strlen(obj), &cfg,
                                  NULL);
  TEST_CHECK(r == TOBJ_OK);
  TEST_CHECK(sc.attrib.vertices.count == 27);
  TEST_CHECK(sc.shapes[0].mesh.num_indices == 9);
  tobj_scene_free_f(&sc);
}

static const char *kMapMtlA =
    "newmtl shared\n"
    "Kd 0.10 0.20 0.30\n"
    "map_Kd -s 2 2 1 shared.png\n"
    "foo bar baz\n"
    "newmtl alpha\n"
    "map_Kd shared.png\n"
    "map_Bump -bm 3 normal.png\n";

static const char *kMapMtlB =
    "newmtl shared\n"
    "Kd 9 9 9\n"
    "map_Kd ignored.png\n"
    "newmtl beta\n"
    "Kd 0.4 0.5 0.6\n"
    "map_Kd beta.png\n";

static const char *kMapObj =
    "mtllib a.mtl b.mtl\n"
    "v 0 0 0\n"
    "v 1 0 0\n"
    "v 0 1 0\n"
    "usemtl shared\n"
    "f 1 2 3\n"
    "usemtl missing\n"
    "f 1 2 3\n"
    "usemtl beta\n"
    "f 1 2 3\n"
    "usemtl shared\n"
    "f 1 2 3\n";

static void test_material_map_autobind_and_dedupe(void) {
  const test_mtl_entry entries[] = {
      {"a.mtl", kMapMtlA},
      {"b.mtl", kMapMtlB},
  };
  test_mtl_set set = {entries, sizeof(entries) / sizeof(entries[0])};
  tobj_load_config cfg = tobj_default_config();
  cfg.mtl_resolver = test_mtl_resolver;
  cfg.mtl_resolver_user_data = &set;

  tobj_scene_f scene;
  tobj_result r =
      tobj_load_obj_from_memory_f(&scene, (const uint8_t *)kMapObj,
                                  strlen(kMapObj), &cfg, NULL);
  TEST_CHECK(r == TOBJ_OK);
  TEST_CHECK(scene.num_materials == 4);
  TEST_CHECK(scene.shapes[0].mesh.num_faces == 4);

  tobj_material_map_f map;
  r = tobj_material_map_build_f(&map, &scene);
  TEST_CHECK(r == TOBJ_OK);
  TEST_CHECK(map.num_materials == 3);
  TEST_CHECK(map.num_source_materials == 4);
  TEST_CHECK(map.source_to_material[0] == 0);
  TEST_CHECK(map.source_to_material[1] == 1);
  TEST_CHECK(map.source_to_material[2] == 0);
  TEST_CHECK(map.source_to_material[3] == 2);

  TEST_CHECK(map.num_shapes == 1);
  TEST_CHECK(map.num_face_material_ids == 4);
  TEST_CHECK(map.shape_face_offsets[0] == 0);
  TEST_CHECK(map.shape_face_offsets[1] == 4);
  TEST_CHECK(map.face_material_ids[0] == 0);
  TEST_CHECK(map.face_material_ids[1] == -1);
  TEST_CHECK(map.face_material_ids[2] == 2);
  TEST_CHECK(map.face_material_ids[3] == 0);

  TEST_CHECK(map.num_textures == 3);
  TEST_CHECK(map.num_texture_slots == 4);
  TEST_CHECK(strcmp(map.textures[0].path, "shared.png") == 0);
  TEST_CHECK(map.textures[0].num_references == 2);
  TEST_CHECK(strcmp(map.textures[1].path, "normal.png") == 0);
  TEST_CHECK(map.textures[1].num_references == 1);
  TEST_CHECK(strcmp(map.textures[2].path, "beta.png") == 0);

  TEST_CHECK(map.texture_slots[0].kind == TOBJ_TEXTURE_SLOT_DIFFUSE);
  TEST_CHECK(map.texture_slots[0].material_id == 0);
  TEST_CHECK(map.texture_slots[0].texture_id == 0);
  TEST_CHECK(map.texture_slots[1].kind == TOBJ_TEXTURE_SLOT_DIFFUSE);
  TEST_CHECK(map.texture_slots[1].material_id == 1);
  TEST_CHECK(map.texture_slots[1].texture_id == 0);
  TEST_CHECK(map.texture_slots[2].kind == TOBJ_TEXTURE_SLOT_BUMP);
  TEST_CHECK(map.texture_slots[2].material_id == 1);
  TEST_CHECK(map.texture_slots[2].texture_id == 1);
  TEST_CHECK(map.texture_slots[3].kind == TOBJ_TEXTURE_SLOT_DIFFUSE);
  TEST_CHECK(map.texture_slots[3].material_id == 2);
  TEST_CHECK(map.texture_slots[3].texture_id == 2);
  TEST_CHECK(map.texture_slots[0].option->scale[0] == 2.0f);
  TEST_CHECK(map.texture_slots[1].option->scale[0] == 1.0f);
  TEST_CHECK(map.texture_slots[2].option->bump_multiplier == 3.0f);

  TEST_CHECK(map.materials[0].diffuse[0] == 0.1f);
  TEST_CHECK(strcmp(map.materials[0].diffuse_texname, "shared.png") == 0);
  TEST_CHECK(tobj_material_map_find_f(&map, "shared") == 0);
  TEST_CHECK(tobj_material_map_find_f(&map, "alpha") == 1);
  TEST_CHECK(tobj_material_map_find_f(&map, "beta") == 2);
  TEST_CHECK(tobj_material_map_find_f(&map, "Shared") == -1);
  TEST_CHECK(tobj_material_map_find_f(&map, "missing") == -1);
  TEST_CHECK(strcmp(tobj_texture_slot_kind_name(TOBJ_TEXTURE_SLOT_NORMAL),
                    "normal") == 0);
  TEST_CHECK(strcmp(tobj_texture_slot_kind_name(TOBJ_TEXTURE_SLOT_COUNT),
                    "unknown") == 0);

  /* The map owns its material strings and arrays, not the source scene. */
  tobj_scene_free_f(&scene);
  TEST_CHECK(strcmp(map.materials[0].name, "shared") == 0);
  TEST_CHECK(strcmp(map.materials[1].name, "alpha") == 0);
  TEST_CHECK(strcmp(map.materials[2].name, "beta") == 0);
  TEST_CHECK(strcmp(map.texture_slots[2].path, "normal.png") == 0);
  TEST_CHECK(map.texture_slots[0].option->scale[0] == 2.0f);
  const char *foo = tobj_material_get_param_f(&map.materials[0], "foo");
  TEST_CHECK(foo && strcmp(foo, "bar baz") == 0);
  tobj_material_map_free_f(&map);
  TEST_CHECK(map.materials == NULL);
  TEST_CHECK(map.num_materials == 0);
  tobj_material_map_free_f(&map); /* idempotent */

  tobj_scene_d scene_d;
  r = tobj_load_obj_from_memory_d(&scene_d, (const uint8_t *)kMapObj,
                                  strlen(kMapObj), &cfg, NULL);
  TEST_CHECK(r == TOBJ_OK);
  tobj_material_map_d map_d;
  TEST_CHECK(tobj_material_map_build_d(&map_d, &scene_d) == TOBJ_OK);
  TEST_CHECK(map_d.num_materials == 3);
  TEST_CHECK(map_d.materials[0].diffuse[0] == 0.1);
  TEST_CHECK(map_d.face_material_ids[2] == 2);
  tobj_material_map_free_d(&map_d);
  tobj_scene_free_d(&scene_d);
}

static void test_material_map_boundaries(void) {
  tobj_material_map_f map;
  TEST_CHECK(tobj_material_map_build_f(NULL, NULL) == TOBJ_ERR_INVALID_ARG);
  TEST_CHECK(tobj_material_map_build_f(&map, NULL) == TOBJ_ERR_INVALID_ARG);
  TEST_CHECK(map.materials == NULL);

  tobj_scene_f empty;
  const char *empty_obj = "";
  load_string_f(empty_obj, &empty, true);
  TEST_CHECK(tobj_material_map_build_f(&map, &empty) == TOBJ_OK);
  TEST_CHECK(map.num_materials == 0);
  TEST_CHECK(map.num_source_materials == 0);
  TEST_CHECK(map.num_shapes == 0);
  TEST_CHECK(map.shape_face_offsets[0] == 0);
  TEST_CHECK(map.num_face_material_ids == 0);
  TEST_CHECK(map.num_texture_slots == 0);
  TEST_CHECK(map.num_textures == 0);
  tobj_material_map_free_f(&map);
  tobj_scene_free_f(&empty);

  const test_mtl_entry entries[] = {
      {"dup.mtl", "newmtl a\nKd 0.1 0.2 0.3\nnewmtl a\nKd 1 1 1\n"},
  };
  test_mtl_set set = {entries, sizeof(entries) / sizeof(entries[0])};
  tobj_load_config cfg = tobj_default_config();
  cfg.mtl_resolver = test_mtl_resolver;
  cfg.mtl_resolver_user_data = &set;
  const char *obj =
      "mtllib dup.mtl\n"
      "v 0 0 0\n"
      "v 1 0 0\n"
      "v 0 1 0\n"
      "g first\n"
      "usemtl a\n"
      "f 1 2 3\n"
      "g second\n"
      "usemtl ghost\n"
      "f 1 2 3\n";
  tobj_scene_f scene;
  tobj_result r = tobj_load_obj_from_memory_f(
      &scene, (const uint8_t *)obj, strlen(obj), &cfg, NULL);
  TEST_CHECK(r == TOBJ_OK);
  TEST_CHECK(scene.num_materials == 2);
  TEST_CHECK(scene.num_shapes == 2);
  TEST_CHECK(tobj_material_map_build_f(&map, &scene) == TOBJ_OK);
  TEST_CHECK(map.num_materials == 1);
  TEST_CHECK(map.num_source_materials == 2);
  TEST_CHECK(map.source_to_material[0] == 0);
  TEST_CHECK(map.source_to_material[1] == 0);
  TEST_CHECK(map.num_shapes == 2);
  TEST_CHECK(map.shape_face_offsets[0] == 0);
  TEST_CHECK(map.shape_face_offsets[1] == 1);
  TEST_CHECK(map.shape_face_offsets[2] == 2);
  TEST_CHECK(map.face_material_ids[0] == 0);
  TEST_CHECK(map.face_material_ids[1] == -1);
  TEST_CHECK(map.materials[0].diffuse[0] == 0.1f);
  tobj_material_map_free_f(&map);
  tobj_scene_free_f(&scene);

  tobj_scene_f malformed;
  memset(&malformed, 0, sizeof malformed);
  malformed.num_materials = 1;
  TEST_CHECK(tobj_material_map_build_f(&map, &malformed) ==
             TOBJ_ERR_INVALID_ARG);
  TEST_CHECK(map.materials == NULL);

  tobj_scene_f constrained;
  memset(&constrained, 0, sizeof constrained);
  constrained.allocator = tobj_default_allocator();
  constrained.allocator.max_alloc_size = 16;
  TEST_CHECK(tobj_material_map_build_f(&map, &constrained) ==
             TOBJ_ERR_ALLOC);
  TEST_CHECK(map.materials == NULL);
  TEST_CHECK(map.arena == NULL);
  tobj_material_map_free_f(NULL);
}

static void test_material_map_reload_10000(void) {
  const test_mtl_entry entries[] = {
      {"a.mtl", kMapMtlA},
      {"b.mtl", kMapMtlB},
  };
  test_mtl_set set = {entries, sizeof(entries) / sizeof(entries[0])};
  alloc_stats stats;
  memset(&stats, 0, sizeof stats);
  tobj_allocator alloc = stats_allocator(&stats);
  uint64_t expected = 0;
  int ok = 1;

  for (int i = 0; i < 10000; i++) {
    tobj_load_config cfg = tobj_default_config();
    cfg.allocator = alloc;
    cfg.mtl_resolver = test_mtl_resolver;
    cfg.mtl_resolver_user_data = &set;
    tobj_scene_f scene;
    tobj_result r =
        tobj_load_obj_from_memory_f(&scene, (const uint8_t *)kMapObj,
                                    strlen(kMapObj), &cfg, NULL);
    if (r != TOBJ_OK) {
      ok = 0;
      break;
    }
    tobj_material_map_f map;
    r = tobj_material_map_build_f(&map, &scene);
    if (r != TOBJ_OK) {
      tobj_scene_free_f(&scene);
      ok = 0;
      break;
    }
    uint64_t digest = material_map_digest(&map);
    if (i == 0) {
      expected = digest;
    } else if (digest != expected) {
      ok = 0;
    }
    tobj_material_map_free_f(&map);
    tobj_scene_free_f(&scene);
    if (stats.live != 0) {
      ok = 0;
      break;
    }
  }

  printf("material-map consistency: iterations=10000 digest=%016llx "
         "allocs=%zu frees=%zu live=%zu\n",
         (unsigned long long)expected, stats.allocs, stats.frees, stats.live);
  TEST_CHECK(ok);
  TEST_CHECK(stats.allocs > 0);
  TEST_CHECK(stats.frees == stats.allocs);
  TEST_CHECK(stats.live == 0);
}

TEST_LIST = {
    {"corpus_loads", test_corpus_loads},
    {"both_precisions", test_both_precisions},
    {"quad_triangulation", test_quad_triangulation},
    {"no_triangulation", test_no_triangulation},
    {"negative_relative_index", test_negative_relative_index},
    {"bom_and_crlf", test_bom_and_crlf},
    {"vertex_colors", test_vertex_colors},
    {"color_fallback_white", test_color_fallback_white},
    {"lines_and_points", test_lines_and_points},
    {"degenerate_face_skipped", test_degenerate_face_skipped},
    {"empty_and_garbage", test_empty_and_garbage},
    {"mtl_standalone", test_mtl_standalone},
    {"io_callbacks", test_io_callbacks},
    {"allocator_limit_failure", test_allocator_limit_failure},
    {"input_limit_failure", test_input_limit_failure},
    {"io_input_limit_exact_and_over", test_io_input_limit_exact_and_over},
    {"io_callback_errors", test_io_callback_errors},
    {"invalid_allocator_rejected", test_invalid_allocator_rejected},
    {"allocator_without_calloc_or_realloc",
     test_allocator_without_calloc_or_realloc},
    {"material_map_autobind_and_dedupe",
     test_material_map_autobind_and_dedupe},
    {"material_map_boundaries", test_material_map_boundaries},
    {"material_map_reload_10000", test_material_map_reload_10000},
    {NULL, NULL}};
