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

#define CAP_VERTICES 64
#define CAP_FACES 64
#define CAP_FACE_ARITY 16

typedef struct captured_events {
  float vertices[CAP_VERTICES * 4];
  size_t num_vertices;
  float vertex_colors[CAP_VERTICES * 7];
  size_t num_vertex_colors;
  float normals[CAP_VERTICES * 3];
  size_t num_normals;
  float texcoords[CAP_VERTICES * 3];
  size_t num_texcoords;
  tobj_index faces[CAP_FACES * CAP_FACE_ARITY];
  int face_arities[CAP_FACES];
  size_t num_faces;
  tobj_index lines[CAP_FACES * CAP_FACE_ARITY];
  int line_arities[CAP_FACES];
  size_t num_lines;
  tobj_index points[CAP_FACES * CAP_FACE_ARITY];
  int point_arities[CAP_FACES];
  size_t num_points;
} captured_events;

static void capture_vertex(void *ud, float x, float y, float z, float w) {
  captured_events *c = (captured_events *)ud;
  if (c->num_vertices >= CAP_VERTICES) return;
  float *v = &c->vertices[c->num_vertices * 4];
  v[0] = x;
  v[1] = y;
  v[2] = z;
  v[3] = w;
  c->num_vertices++;
}

static void capture_vertex_color(void *ud, float x, float y, float z, float r,
                                 float g, float b, bool has_color) {
  captured_events *c = (captured_events *)ud;
  if (c->num_vertex_colors >= CAP_VERTICES) return;
  float *v = &c->vertex_colors[c->num_vertex_colors * 7];
  v[0] = x;
  v[1] = y;
  v[2] = z;
  v[3] = r;
  v[4] = g;
  v[5] = b;
  v[6] = has_color ? 1.0f : 0.0f;
  c->num_vertex_colors++;
}

static void capture_normal(void *ud, float x, float y, float z) {
  captured_events *c = (captured_events *)ud;
  if (c->num_normals >= CAP_VERTICES) return;
  float *v = &c->normals[c->num_normals * 3];
  v[0] = x;
  v[1] = y;
  v[2] = z;
  c->num_normals++;
}

static void capture_texcoord(void *ud, float x, float y, float z) {
  captured_events *c = (captured_events *)ud;
  if (c->num_texcoords >= CAP_VERTICES) return;
  float *v = &c->texcoords[c->num_texcoords * 3];
  v[0] = x;
  v[1] = y;
  v[2] = z;
  c->num_texcoords++;
}

static void capture_face(void *ud, const tobj_index *indices, int num) {
  captured_events *c = (captured_events *)ud;
  if (c->num_faces >= CAP_FACES || num < 0 || num > CAP_FACE_ARITY) return;
  memcpy(&c->faces[c->num_faces * CAP_FACE_ARITY], indices,
         (size_t)num * sizeof(*indices));
  c->face_arities[c->num_faces] = num;
  c->num_faces++;
}

static void capture_line(void *ud, const tobj_index *indices, int num) {
  captured_events *c = (captured_events *)ud;
  if (c->num_lines >= CAP_FACES || num < 0 || num > CAP_FACE_ARITY) return;
  memcpy(&c->lines[c->num_lines * CAP_FACE_ARITY], indices,
         (size_t)num * sizeof(*indices));
  c->line_arities[c->num_lines] = num;
  c->num_lines++;
}

static void capture_point(void *ud, const tobj_index *indices, int num) {
  captured_events *c = (captured_events *)ud;
  if (c->num_points >= CAP_FACES || num < 0 || num > CAP_FACE_ARITY) return;
  memcpy(&c->points[c->num_points * CAP_FACE_ARITY], indices,
         (size_t)num * sizeof(*indices));
  c->point_arities[c->num_points] = num;
  c->num_points++;
}

static tobj_callbacks_f capture_callbacks(void *ud) {
  tobj_callbacks_f cb;
  memset(&cb, 0, sizeof(cb));
  cb.vertex_cb = capture_vertex;
  cb.vertex_color_cb = capture_vertex_color;
  cb.normal_cb = capture_normal;
  cb.texcoord_cb = capture_texcoord;
  cb.index_cb = capture_face;
  cb.line_cb = capture_line;
  cb.point_cb = capture_point;
  cb.user_data = ud;
  return cb;
}

static void check_events_equal(const captured_events *a,
                               const captured_events *b) {
  TEST_CHECK(a->num_vertices == b->num_vertices);
  TEST_CHECK(a->num_vertex_colors == b->num_vertex_colors);
  TEST_CHECK(a->num_normals == b->num_normals);
  TEST_CHECK(a->num_texcoords == b->num_texcoords);
  TEST_CHECK(a->num_faces == b->num_faces);
  TEST_CHECK(a->num_lines == b->num_lines);
  TEST_CHECK(a->num_points == b->num_points);
  TEST_CHECK(memcmp(a->vertices, b->vertices,
                    a->num_vertices * 4 * sizeof(float)) == 0);
  TEST_CHECK(memcmp(a->vertex_colors, b->vertex_colors,
                    a->num_vertex_colors * 7 * sizeof(float)) == 0);
  TEST_CHECK(memcmp(a->normals, b->normals,
                    a->num_normals * 3 * sizeof(float)) == 0);
  TEST_CHECK(memcmp(a->texcoords, b->texcoords,
                    a->num_texcoords * 3 * sizeof(float)) == 0);
  TEST_CHECK(memcmp(a->faces, b->faces,
                    a->num_faces * CAP_FACE_ARITY * sizeof(tobj_index)) == 0);
  TEST_CHECK(memcmp(a->face_arities, b->face_arities,
                    a->num_faces * sizeof(int)) == 0);
  TEST_CHECK(memcmp(a->lines, b->lines,
                    a->num_lines * CAP_FACE_ARITY * sizeof(tobj_index)) == 0);
  TEST_CHECK(memcmp(a->line_arities, b->line_arities,
                    a->num_lines * sizeof(int)) == 0);
  TEST_CHECK(memcmp(a->points, b->points,
                    a->num_points * CAP_FACE_ARITY * sizeof(tobj_index)) == 0);
  TEST_CHECK(memcmp(a->point_arities, b->point_arities,
                    a->num_points * sizeof(int)) == 0);
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

/* ---- streaming API ----------------------------------------------------- */

static tobj_result stream_capture_f(const uint8_t *data, size_t len,
                                    const tobj_load_config *cfg,
                                    size_t chunk, captured_events *out,
                                    tobj_stream_stats *stats) {
  memset(out, 0, sizeof(*out));
  tobj_callbacks_f cb = capture_callbacks(out);
  tobj_stream_f *stream = NULL;
  tobj_result r = tobj_stream_create_f(&stream, &cb, cfg, NULL);
  if (r != TOBJ_OK) return r;

  if (chunk == 0) chunk = len ? len : 1;
  size_t pos = 0;
  while (pos < len) {
    size_t n = len - pos;
    if (n > chunk) n = chunk;
    r = tobj_stream_feed_f(stream, data + pos, n);
    if (r != TOBJ_OK) break;
    pos += n;
  }
  if (r == TOBJ_OK) r = tobj_stream_finish_f(stream);
  if (r == TOBJ_OK && stats) r = tobj_stream_get_stats_f(stream, stats);
  tobj_stream_destroy_f(stream);
  return r;
}

static void test_stream_matches_callback_loader(void) {
  static const char obj[] =
      "\xEF\xBB\xBF# split me anywhere\r\n"
      "v 0 0 0\r\n"
      "v 1 0 0 1\r\n"
      "v 1 1 0 1 0 0\r\n"
      "v 0 1 0\n"
      "vn 0 0 1\n"
      "vt 0 0 1\n"
      "vt 1 0\n"
      "g first second\n"
      "o streamed\n"
      "s 2\n"
      "usemtl material\n"
      "f 1/1/1 2/2/1 3/1/1 4/2/1\n"
      "f -4 -3 -2\n"
      "l 1 2 3\n"
      "p 4\n"
      "p\n"
      "f 1 2\n"
      "f 0 1 2\n"
      "f -99 1 2\n"
      "unknown directive ignored\n";
  const size_t len = sizeof(obj) - 1;

  tobj_load_config cfg = tobj_default_config();
  captured_events full;
  memset(&full, 0, sizeof(full));
  tobj_callbacks_f cb = capture_callbacks(&full);
  TEST_CHECK(tobj_load_obj_with_callbacks_f(
                 (const uint8_t *)obj, len, &cb, &cfg, NULL) == TOBJ_OK);

  static const size_t chunks[] = {1, 2, 3, 4, 5, 7, 11, 16, 31, 64, len + 1};
  for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
    captured_events got;
    tobj_stream_stats stats;
    tobj_result r =
        stream_capture_f((const uint8_t *)obj, len, &cfg, chunks[i], &got,
                         &stats);
    TEST_CHECK_(r == TOBJ_OK, "chunk %zu -> %s", chunks[i],
                tobj_result_string(r));
    check_events_equal(&full, &got);
    TEST_CHECK(stats.input_bytes == len);
    TEST_CHECK(stats.vertices == full.num_vertices);
    TEST_CHECK(stats.normals == full.num_normals);
    TEST_CHECK(stats.texcoords == full.num_texcoords);
    TEST_CHECK(stats.faces == full.num_faces);
  }

  /* Exhaustive two-chunk split, including every BOM/CRLF boundary. */
  for (size_t split = 0; split <= len; split++) {
    captured_events got;
    memset(&got, 0, sizeof(got));
    tobj_callbacks_f scb = capture_callbacks(&got);
    tobj_stream_f *stream = NULL;
    tobj_result r = tobj_stream_create_f(&stream, &scb, &cfg, NULL);
    TEST_CHECK(r == TOBJ_OK);
    if (r != TOBJ_OK) continue;
    r = tobj_stream_feed_f(stream, (const uint8_t *)obj, split);
    if (r == TOBJ_OK)
      r = tobj_stream_feed_f(stream, (const uint8_t *)obj + split,
                             len - split);
    if (r == TOBJ_OK) r = tobj_stream_finish_f(stream);
    tobj_stream_destroy_f(stream);
    TEST_CHECK_(r == TOBJ_OK, "split %zu -> %s", split,
                tobj_result_string(r));
    check_events_equal(&full, &got);
  }
}

static void test_stream_scene_parity_raw_faces(void) {
  const char *obj =
      "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
      "f 1 2 3 4\n"
      "f -4/-4 -3/-3 -2/-2\n";
  tobj_load_config cfg = tobj_default_config();
  cfg.triangulate = false;

  tobj_scene_f scene;
  TEST_CHECK(tobj_load_obj_from_memory_f(
                 &scene, (const uint8_t *)obj, strlen(obj), &cfg, NULL) ==
             TOBJ_OK);
  TEST_CHECK(scene.num_shapes == 1);

  captured_events got;
  tobj_result r =
      stream_capture_f((const uint8_t *)obj, strlen(obj), &cfg, 2, &got, NULL);
  TEST_CHECK(r == TOBJ_OK);
  TEST_CHECK(got.num_faces == scene.shapes[0].mesh.num_faces);
  TEST_CHECK(got.num_faces == 2);
  if (got.num_faces == 2) {
    TEST_CHECK(got.face_arities[0] == 4);
    TEST_CHECK(got.face_arities[1] == 3);
    TEST_CHECK(memcmp(got.faces, scene.shapes[0].mesh.indices,
                      4 * sizeof(tobj_index)) == 0);
    TEST_CHECK(memcmp(&got.faces[CAP_FACE_ARITY],
                      &scene.shapes[0].mesh.indices[4],
                      3 * sizeof(tobj_index)) == 0);
  }
  tobj_scene_free_f(&scene);
}

static void test_stream_edge_cases(void) {
  tobj_load_config cfg = tobj_default_config();
  tobj_callbacks_f cb;
  memset(&cb, 0, sizeof(cb));
  tobj_stream_f *stream = NULL;
  tobj_stream_stats stats;

  /* Empty stream, NULL/zero feed, idempotent finish, feed after finish. */
  TEST_CHECK(tobj_stream_create_f(&stream, &cb, &cfg, NULL) == TOBJ_OK);
  TEST_CHECK(tobj_stream_feed_f(stream, NULL, 0) == TOBJ_OK);
  TEST_CHECK(tobj_stream_finish_f(stream) == TOBJ_OK);
  TEST_CHECK(tobj_stream_finish_f(stream) == TOBJ_OK);
  TEST_CHECK(tobj_stream_get_stats_f(stream, &stats) == TOBJ_OK);
  TEST_CHECK(stats.input_bytes == 0 && stats.lines == 0);
  TEST_CHECK(tobj_stream_feed_f(stream, (const uint8_t *)"x", 1) ==
             TOBJ_ERR_INVALID_ARG);
  tobj_stream_destroy_f(stream);

  TEST_CHECK(tobj_stream_create_f(NULL, &cb, &cfg, NULL) ==
             TOBJ_ERR_INVALID_ARG);
  TEST_CHECK(tobj_stream_create_f(&stream, NULL, &cfg, NULL) ==
             TOBJ_ERR_INVALID_ARG);
  TEST_CHECK(tobj_stream_feed_f(NULL, NULL, 0) == TOBJ_ERR_INVALID_ARG);
  TEST_CHECK(tobj_stream_finish_f(NULL) == TOBJ_ERR_INVALID_ARG);
  TEST_CHECK(tobj_stream_get_stats_f(NULL, &stats) == TOBJ_ERR_INVALID_ARG);
  tobj_stream_destroy_f(NULL);

  /* Unterminated final line and lone-CR line separators. */
  {
    const char obj[] = "v 0 0 0\rv 1 0 0\rv 0 1 0\rf 1 2 3";
    captured_events got;
    TEST_CHECK(stream_capture_f((const uint8_t *)obj, sizeof(obj) - 1, &cfg, 1,
                                &got, &stats) == TOBJ_OK);
    TEST_CHECK(stats.vertices == 3);
    TEST_CHECK(stats.faces == 1);
    TEST_CHECK(got.num_faces == 1 && got.face_arities[0] == 3);
  }

  /* Empty physical lines, including a BOM-only line. */
  {
    const char obj[] = "\n\r\n\r\xEF\xBB\xBF\nv 0 0 0\n\n";
    captured_events got;
    TEST_CHECK(stream_capture_f((const uint8_t *)obj, sizeof(obj) - 1, &cfg,
                                1, &got, &stats) == TOBJ_OK);
    TEST_CHECK(stats.lines == 6);
    TEST_CHECK(stats.vertices == 1);
  }

  /* A stream containing only the BOM has zero lines, matching the full loader. */
  {
    const char obj[] = "\xEF\xBB\xBF";
    captured_events got;
    TEST_CHECK(stream_capture_f((const uint8_t *)obj, sizeof(obj) - 1, &cfg, 1,
                                &got, &stats) == TOBJ_OK);
    TEST_CHECK(stats.lines == 0);
    TEST_CHECK(stats.vertices == 0);
  }

  /* A BOM in the middle of the stream is ordinary data, not stripped. */
  {
    const char obj[] = "v 0 0 0\n\xEF\xBB\xBFv 1 0 0\n";
    captured_events got;
    TEST_CHECK(stream_capture_f((const uint8_t *)obj, sizeof(obj) - 1, &cfg, 2,
                                &got, &stats) == TOBJ_OK);
    TEST_CHECK(stats.vertices == 1);
  }

  /* Input, line, arity, vertex, face, and index limits. */
  {
    tobj_load_config lim = tobj_default_config();
    lim.max_input_bytes = 5;
    captured_events got;
    const char input[] = "v 0 0 0\n";
    TEST_CHECK(stream_capture_f((const uint8_t *)input, sizeof(input) - 1,
                                &lim, 3, &got, NULL) ==
               TOBJ_ERR_LIMIT_EXCEEDED);
  }
  {
    tobj_load_config lim = tobj_default_config();
    lim.max_line_bytes = 6;
    captured_events got;
    const char long_line[] = "v 0 0 0\n";
    const char exact_line[] = "v 0 0\n";
    TEST_CHECK(stream_capture_f((const uint8_t *)long_line,
                                sizeof(long_line) - 1, &lim, 2, &got, NULL) ==
               TOBJ_ERR_LIMIT_EXCEEDED);
    TEST_CHECK(stream_capture_f((const uint8_t *)exact_line,
                                sizeof(exact_line) - 1, &lim, 2, &got, NULL) ==
               TOBJ_OK);
  }
  {
    tobj_load_config lim = tobj_default_config();
    lim.max_face_arity = 3;
    captured_events got;
    const char input[] =
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n";
    TEST_CHECK(stream_capture_f((const uint8_t *)input, sizeof(input) - 1,
                                &lim, 4, &got, NULL) ==
               TOBJ_ERR_LIMIT_EXCEEDED);
  }
  {
    tobj_load_config lim = tobj_default_config();
    lim.max_vertices = 2;
    captured_events got;
    const char input[] = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
    TEST_CHECK(stream_capture_f((const uint8_t *)input, sizeof(input) - 1,
                                &lim, 3, &got, NULL) ==
               TOBJ_ERR_LIMIT_EXCEEDED);
  }
  {
    tobj_load_config lim = tobj_default_config();
    lim.max_faces = 1;
    const char two_faces[] =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\nf 1 2 3\n";
    captured_events got;
    TEST_CHECK(stream_capture_f((const uint8_t *)two_faces,
                                sizeof(two_faces) - 1, &lim, 3, &got, NULL) ==
               TOBJ_ERR_LIMIT_EXCEEDED);
  }
  {
    tobj_load_config lim = tobj_default_config();
    lim.max_indices = 3;
    const char two_faces[] =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\nf 1 2 3\n";
    captured_events got;
    TEST_CHECK(stream_capture_f((const uint8_t *)two_faces,
                                sizeof(two_faces) - 1, &lim, 3, &got, NULL) ==
               TOBJ_ERR_LIMIT_EXCEEDED);
  }
}

typedef struct counting_allocator {
  size_t live_bytes;
  size_t peak_bytes;
  size_t allocations;
  size_t frees;
} counting_allocator;

static void *counting_alloc(void *ud, size_t size, size_t align) {
  (void)align;
  counting_allocator *a = (counting_allocator *)ud;
  void *p = malloc(size ? size : 1);
  if (!p) return NULL;
  a->live_bytes += size;
  a->allocations++;
  if (a->live_bytes > a->peak_bytes) a->peak_bytes = a->live_bytes;
  return p;
}

static void *counting_calloc(void *ud, size_t count, size_t size,
                             size_t align) {
  (void)align;
  counting_allocator *a = (counting_allocator *)ud;
  void *p = calloc(count ? count : 1, size ? size : 1);
  if (!p) return NULL;
  size_t bytes = (count ? count : 1) * (size ? size : 1);
  a->live_bytes += bytes;
  a->allocations++;
  if (a->live_bytes > a->peak_bytes) a->peak_bytes = a->live_bytes;
  return p;
}

static void counting_free(void *ud, void *ptr, size_t size) {
  counting_allocator *a = (counting_allocator *)ud;
  if (!ptr) return;
  if (size > a->live_bytes) {
    a->live_bytes = 0;
  } else {
    a->live_bytes -= size;
  }
  a->frees++;
  free(ptr);
}

static void test_stream_1000_lifecycles_no_leak(void) {
  const char obj[] =
      "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
      "f 1 2 3 4\nf -4 -3 -2\n";
  const size_t len = sizeof(obj) - 1;
  counting_allocator ca;
  memset(&ca, 0, sizeof(ca));
  tobj_load_config cfg = tobj_default_config();
  cfg.allocator.alloc = counting_alloc;
  cfg.allocator.calloc = counting_calloc;
  cfg.allocator.realloc = NULL;
  cfg.allocator.free = counting_free;
  cfg.allocator.user_data = &ca;
  tobj_callbacks_f cb;
  memset(&cb, 0, sizeof(cb));

  int failed = 0;
  for (int iter = 0; iter < 1000; iter++) {
    tobj_stream_f *stream = NULL;
    tobj_result r = tobj_stream_create_f(&stream, &cb, &cfg, NULL);
    if (r != TOBJ_OK) {
      failed = 1;
      break;
    }
    size_t pos = 0;
    size_t chunk = 1 + (size_t)(iter % 7);
    while (pos < len && r == TOBJ_OK) {
      size_t n = len - pos;
      if (n > chunk) n = chunk;
      r = tobj_stream_feed_f(stream, (const uint8_t *)obj + pos, n);
      pos += n;
    }
    if (r == TOBJ_OK) r = tobj_stream_finish_f(stream);
    tobj_stream_destroy_f(stream);
    if (r != TOBJ_OK || ca.live_bytes != 0) {
      failed = 1;
      break;
    }
  }
  TEST_CHECK(failed == 0);
  TEST_CHECK(ca.live_bytes == 0);
  TEST_CHECK(ca.allocations == ca.frees);
}

static void test_stream_memory_bound(void) {
  tobj_load_config cfg = tobj_default_config();
  cfg.max_line_bytes = 64;
  cfg.max_face_arity = 8;
  tobj_callbacks_f cb;
  memset(&cb, 0, sizeof(cb));
  tobj_stream_f *stream = NULL;
  TEST_CHECK(tobj_stream_create_f(&stream, &cb, &cfg, NULL) == TOBJ_OK);

  static const char vertices[] =
      "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
      "v 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n";
  TEST_CHECK(tobj_stream_feed_f(stream, (const uint8_t *)vertices,
                                sizeof(vertices) - 1) == TOBJ_OK);

  static const char face[] = "f 1 2 3 4 5 6 7 8\n";
  for (int i = 0; i < 100000; i++) {
    if (tobj_stream_feed_f(stream, (const uint8_t *)face,
                           sizeof(face) - 1) != TOBJ_OK) {
      TEST_CHECK(0);
      break;
    }
  }
  TEST_CHECK(tobj_stream_finish_f(stream) == TOBJ_OK);

  tobj_stream_stats stats;
  TEST_CHECK(tobj_stream_get_stats_f(stream, &stats) == TOBJ_OK);
  tobj_stream_destroy_f(stream);
  TEST_CHECK(stats.faces == 100000);
  TEST_CHECK(stats.max_line_bytes <= 64);
  TEST_CHECK(stats.max_face_arity == 8);
  size_t bound = (64 + 4) + 8 * sizeof(tobj_index);
  TEST_CHECK_(stats.peak_buffer_bytes <= bound,
              "peak=%zu bound=%zu", stats.peak_buffer_bytes, bound);
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
    {"stream_matches_callback_loader", test_stream_matches_callback_loader},
    {"stream_scene_parity_raw_faces", test_stream_scene_parity_raw_faces},
    {"stream_edge_cases", test_stream_edge_cases},
    {"stream_1000_lifecycles_no_leak", test_stream_1000_lifecycles_no_leak},
    {"stream_memory_bound", test_stream_memory_bound},
    {NULL, NULL}};
