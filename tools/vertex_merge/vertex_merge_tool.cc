// Parse-stage vertex welding tool.
//
// Loads an .obj with tinyobjloader, expands every face corner into an
// explicit (position, normal, texcoord) vertex, welds identical vertices
// with an open-addressing hash table, rebuilds the index buffer, renders
// the mesh before and after with a deterministic software rasterizer, and
// reports vertex/index counts, timing and pixel differences.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include "rasterizer.h"
#include "weld_vertices.h"

namespace vm = vertexmerge;

namespace {

struct Scene {
  std::vector<vm::Vertex> corners;  // one entry per triangle corner
  size_t triangle_count = 0;
  float bbox_min[3] = {std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::infinity()};
  float bbox_max[3] = {-std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity()};
};

vm::Vertex MakeVertex(const tinyobj::attrib_t& attrib,
                      const tinyobj::index_t& index) {
  vm::Vertex v{};
  const float* positions = attrib.vertices.data();
  const float* normals =
      attrib.normals.empty() ? nullptr : attrib.normals.data();
  const float* texcoords =
      attrib.texcoords.empty() ? nullptr : attrib.texcoords.data();

  int pi = index.vertex_index;
  v.px = positions[3 * pi + 0];
  v.py = positions[3 * pi + 1];
  v.pz = positions[3 * pi + 2];

  if (index.normal_index >= 0 && normals) {
    int ni = index.normal_index;
    v.nx = normals[3 * ni + 0];
    v.ny = normals[3 * ni + 1];
    v.nz = normals[3 * ni + 2];
  } else {
    v.nx = 0.0f;
    v.ny = 1.0f;
    v.nz = 0.0f;
  }

  if (index.texcoord_index >= 0 && texcoords) {
    int ti = index.texcoord_index;
    v.u = texcoords[2 * ti + 0];
    v.v = texcoords[2 * ti + 1];
  }

  return v;
}

void ExtendBBox(Scene* scene, const vm::Vertex& v) {
  float p[3] = {v.px, v.py, v.pz};
  for (int k = 0; k < 3; ++k) {
    scene->bbox_min[k] = std::min(scene->bbox_min[k], p[k]);
    scene->bbox_max[k] = std::max(scene->bbox_max[k], p[k]);
  }
}

// Mirrors tinyobjloader's own fan triangulation for polygon faces so the
// corner stream matches the parsed triangle soup.
void AppendFace(const tinyobj::attrib_t& attrib,
                const tinyobj::mesh_t& mesh, size_t face_offset,
                unsigned int face_vertex_count, Scene* scene) {
  for (unsigned int k = 1; k + 1 < face_vertex_count; ++k) {
    const tinyobj::index_t& i0 = mesh.indices[face_offset];
    const tinyobj::index_t& i1 = mesh.indices[face_offset + k];
    const tinyobj::index_t& i2 = mesh.indices[face_offset + k + 1];
    vm::Vertex tri[3] = {MakeVertex(attrib, i0), MakeVertex(attrib, i1),
                         MakeVertex(attrib, i2)};
    for (const vm::Vertex& v : tri) {
      scene->corners.push_back(v);
      ExtendBBox(scene, v);
    }
    ++scene->triangle_count;
  }
}

vm::Camera FitCamera(const Scene& scene) {
  float center[3];
  float radius = 0.0f;
  for (int k = 0; k < 3; ++k) {
    center[k] = (scene.bbox_min[k] + scene.bbox_max[k]) * 0.5f;
    radius = std::max(radius,
                      (scene.bbox_max[k] - scene.bbox_min[k]) * 0.5f);
  }
  radius = std::max(radius, 1e-4f);
  float distance = radius / std::tan(30.0f * 3.14159265358979323846f /
                                     180.0f) *
                   1.0f;
  vm::Camera camera;
  float dir[3] = {0.62f, 0.48f, 0.62f};
  float dir_len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] +
                            dir[2] * dir[2]);
  for (int k = 0; k < 3; ++k) {
    camera.eye[k] = center[k] + dir[k] / dir_len * distance;
    camera.target[k] = center[k];
  }
  camera.fov_y_radians = 60.0f * 3.14159265358979323846f / 180.0f;
  return camera;
}

long CountDiffPixels(const vm::Image& a, const vm::Image& b) {
  if (a.width != b.width || a.height != b.height ||
      a.rgb.size() != b.rgb.size()) {
    return -1;
  }
  long diff = 0;
  for (size_t i = 0; i < a.rgb.size(); ++i) {
    if (a.rgb[i] != b.rgb[i]) {
      ++diff;
    }
  }
  return diff;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s model.obj [out_dir] [width] [height]\n",
                 argv[0]);
    return 2;
  }
  const char* model_path = argv[1];
  std::string out_dir = argc >= 3 ? argv[2] : ".";
  std::error_code mkdir_error;
  std::filesystem::create_directories(out_dir, mkdir_error);
  int width = argc >= 4 ? std::stoi(argv[3]) : 1024;
  int height = argc >= 5 ? std::stoi(argv[4]) : 768;

  auto t_parse_start = std::chrono::steady_clock::now();
  tinyobj::attrib_t attrib;
  std::vector<tinyobj::shape_t> shapes;
  std::vector<tinyobj::material_t> materials;
  std::string warn;
  std::string err;
  if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
                        model_path)) {
    std::fprintf(stderr, "LoadObj failed: %s\n", err.c_str());
    return 1;
  }

  Scene scene;
  for (const tinyobj::shape_t& shape : shapes) {
    size_t offset = 0;
    for (unsigned int face_size : shape.mesh.num_face_vertices) {
      AppendFace(attrib, shape.mesh, offset, face_size, &scene);
      offset += face_size;
    }
  }
  auto t_parse_end = std::chrono::steady_clock::now();

  auto t_weld_start = std::chrono::steady_clock::now();
  vm::WeldResult welded = vm::WeldVertices(scene.corners);
  auto t_weld_end = std::chrono::steady_clock::now();

  // Identity index buffer: the "before" mesh is pure triangle soup.
  std::vector<uint32_t> identity(scene.corners.size());
  for (uint32_t i = 0; i < identity.size(); ++i) {
    identity[i] = i;
  }

  // Correctness checks for the rebuilt index table.
  bool connectivity_ok = welded.indices.size() == scene.corners.size();
  for (size_t i = 0; connectivity_ok && i < welded.indices.size(); ++i) {
    uint32_t new_index = welded.indices[i];
    if (new_index >= welded.vertices.size() ||
        !(welded.vertices[new_index] == scene.corners[i])) {
      connectivity_ok = false;
    }
  }

  vm::Camera camera = FitCamera(scene);
  vm::Image before = vm::Render(scene.corners, identity,
                                scene.triangle_count, camera, width,
                                height);
  vm::Image after = vm::Render(welded.vertices, welded.indices,
                               scene.triangle_count, camera, width,
                               height);
  long diff_pixels = CountDiffPixels(before, after);

  double parse_ms =
      std::chrono::duration<double, std::milli>(t_parse_end -
                                                t_parse_start)
          .count();
  double weld_ms =
      std::chrono::duration<double, std::milli>(t_weld_end -
                                                t_weld_start)
          .count();

  size_t vertices_before = scene.corners.size();
  size_t vertices_after = welded.vertices.size();
  size_t indices_before = scene.corners.size();
  size_t indices_after = welded.indices.size();
  double reduction =
      100.0 *
      (static_cast<double>(vertices_before) -
       static_cast<double>(vertices_after)) /
      static_cast<double>(vertices_before);
  double overhead = 100.0 * weld_ms / parse_ms;

  std::string before_path = out_dir + "/before.ppm";
  std::string after_path = out_dir + "/after.ppm";
  if (!vm::WriteBinaryPpm(before, before_path.c_str()) ||
      !vm::WriteBinaryPpm(after, after_path.c_str())) {
    std::fprintf(stderr, "failed to write PPM outputs into %s\n",
                 out_dir.c_str());
    return 2;
  }

  FILE* stats = std::fopen((out_dir + "/stats.json").c_str(), "w");
  if (!stats) {
    std::fprintf(stderr, "cannot open %s/stats.json for writing\n",
                 out_dir.c_str());
    return 2;
  }
  std::fprintf(stats,
               "{\n"
               "  \"model\": \"%s\",\n"
               "  \"vertex_count_before\": %zu,\n"
               "  \"vertex_count_after\": %zu,\n"
               "  \"vertex_reduction_pct\": %.2f,\n"
               "  \"index_count_before\": %zu,\n"
               "  \"index_count_after\": %zu,\n"
               "  \"triangle_count\": %zu,\n"
               "  \"diff_pixels\": %ld,\n"
               "  \"connectivity_ok\": %s,\n"
               "  \"parse_ms\": %.4f,\n"
               "  \"weld_ms\": %.4f,\n"
               "  \"weld_over_parse_pct\": %.2f,\n"
               "  \"image_size\": \"%dx%d\"\n"
               "}\n",
               model_path, vertices_before, vertices_after, reduction,
               indices_before, indices_after, scene.triangle_count,
               diff_pixels, connectivity_ok ? "true" : "false", parse_ms,
               weld_ms, overhead, width, height);
  std::fclose(stats);

  FILE* stats_txt = std::fopen((out_dir + "/stats.txt").c_str(), "w");
  if (stats_txt) {
    std::fprintf(stats_txt,
                 "model=%s\n"
                 "vertex_count_before=%zu\n"
                 "vertex_count_after=%zu\n"
                 "vertex_reduction_pct=%.2f\n"
                 "index_count_before=%zu\n"
                 "index_count_after=%zu\n"
                 "triangle_count=%zu\n"
                 "diff_pixels=%ld\n"
                 "connectivity_ok=%s\n"
                 "parse_ms=%.4f\n"
                 "weld_ms=%.4f\n"
                 "weld_over_parse_pct=%.2f\n",
                 model_path, vertices_before, vertices_after, reduction,
                 indices_before, indices_after, scene.triangle_count,
                 diff_pixels, connectivity_ok ? "true" : "false",
                 parse_ms, weld_ms, overhead);
    std::fclose(stats_txt);
  }

  std::printf("vertices: %zu -> %zu (%.2f%% reduction)\n",
              vertices_before, vertices_after, reduction);
  std::printf("indices:   %zu -> %zu\n", indices_before, indices_after);
  std::printf("triangles: %zu\n", scene.triangle_count);
  std::printf("diff pixels: %ld, connectivity ok: %s\n", diff_pixels,
              connectivity_ok ? "yes" : "no");
  std::printf("parse: %.4f ms, weld: %.4f ms (%.2f%% of parse)\n",
              parse_ms, weld_ms, overhead);
  return (diff_pixels == 0 && connectivity_ok) ? 0 : 3;
}
