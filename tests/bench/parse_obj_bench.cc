// Standalone OBJ parser benchmark with a field-sensitive output digest.
//
// Generate a deterministic triangle-soup model:
//   parse_obj_bench generate <triangles> <file>
//
// Parse it and print wall time, output digest, and peak RSS:
//   /usr/bin/time -v parse_obj_bench parse <file> [num_threads]
//
// The digest covers every parsed output array, so two loader builds can be
// compared without serializing the full result.
#define TINYOBJLOADER_IMPLEMENTATION
#ifndef TINYOBJLOADER_USE_MMAP
#define TINYOBJLOADER_USE_MMAP
#endif
#include "../../tiny_obj_loader.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace {

static bool Generate(const char *path, size_t triangle_count) {
  FILE *fp = std::fopen(path, "wb");
  if (!fp) {
    std::perror(path);
    return false;
  }

  size_t next_vertex = 1;
  for (size_t i = 0; i < triangle_count; ++i) {
    const unsigned long long base =
        static_cast<unsigned long long>(i);
    std::fprintf(fp,
                 "v %llu.125 0.5 0.25\n"
                 "v %llu.125 1.5 0.25\n"
                 "v %llu.125 0.5 1.25\n"
                 "f %llu %llu %llu\n",
                 base, base, base, static_cast<unsigned long long>(next_vertex),
                 static_cast<unsigned long long>(next_vertex + 1),
                 static_cast<unsigned long long>(next_vertex + 2));
    next_vertex += 3;
  }

  if (std::fclose(fp) != 0) {
    std::perror(path);
    return false;
  }
  return true;
}

static uint64_t HashBytes(uint64_t h, const void *data, size_t bytes) {
  const unsigned char *p = static_cast<const unsigned char *>(data);
  for (size_t i = 0; i < bytes; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

template <typename T>
static uint64_t HashVector(uint64_t h, const std::vector<T> &v) {
  const uint64_t n = static_cast<uint64_t>(v.size());
  h = HashBytes(h, &n, sizeof(n));
  if (!v.empty()) h = HashBytes(h, v.data(), v.size() * sizeof(T));
  return h;
}

static uint64_t Digest(const tinyobj::basic_attrib_t<> &attrib,
                       const std::vector<tinyobj::basic_shape_t<>> &shapes,
                       const std::vector<tinyobj::material_t> &materials) {
  uint64_t h = 1469598103934665603ULL;
  h = HashVector(h, attrib.vertices);
  h = HashVector(h, attrib.vertex_weights);
  h = HashVector(h, attrib.normals);
  h = HashVector(h, attrib.texcoords);
  h = HashVector(h, attrib.texcoord_ws);
  h = HashVector(h, attrib.colors);
  h = HashVector(h, attrib.indices);
  h = HashVector(h, attrib.face_num_verts);
  h = HashVector(h, attrib.material_ids);
  const uint64_t shape_count = static_cast<uint64_t>(shapes.size());
  h = HashBytes(h, &shape_count, sizeof(shape_count));
  for (size_t i = 0; i < shapes.size(); ++i) {
    h = HashVector(h, shapes[i].mesh.indices);
    h = HashVector(h, shapes[i].mesh.num_face_vertices);
    h = HashVector(h, shapes[i].mesh.material_ids);
    h = HashVector(h, shapes[i].mesh.smoothing_group_ids);
  }
  const uint64_t material_count = static_cast<uint64_t>(materials.size());
  h = HashBytes(h, &material_count, sizeof(material_count));
  return h;
}

static long PeakRssKiB() {
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) != 0) return -1;
  return usage.ru_maxrss;
}

static int Parse(const char *path, int num_threads) {
  tinyobj::OptLoadConfig config;
  config.num_threads = num_threads;
  config.triangulate = true;

  tinyobj::basic_attrib_t<> attrib;
  std::vector<tinyobj::basic_shape_t<>> shapes;
  std::vector<tinyobj::material_t> materials;
  std::string warn;
  std::string err;

  const auto begin = std::chrono::steady_clock::now();
  const bool ok = tinyobj::LoadObjOpt(
      &attrib, &shapes, &materials, &warn, &err, path, nullptr, config);
  const auto end = std::chrono::steady_clock::now();
  const double seconds =
      std::chrono::duration<double>(end - begin).count();

  if (!ok) {
    std::fprintf(stderr, "parse failed: %s\n", err.c_str());
    return 1;
  }

  std::printf("ok=%d seconds=%.6f peak_rss_kib=%ld digest=%016llx\n",
              ok ? 1 : 0, seconds, PeakRssKiB(),
              static_cast<unsigned long long>(Digest(attrib, shapes, materials)));
  std::printf("vertices=%zu indices=%zu faces=%zu shapes=%zu materials=%zu\n",
              attrib.vertices.size() / 3, attrib.indices.size(),
              attrib.face_num_verts.size(), shapes.size(), materials.size());
  if (!warn.empty()) std::fprintf(stderr, "warning: %s\n", warn.c_str());
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc == 4 && std::strcmp(argv[1], "generate") == 0) {
    const size_t count = static_cast<size_t>(std::strtoull(argv[2], nullptr, 10));
    return Generate(argv[3], count) ? 0 : 1;
  }
  if ((argc == 3 || argc == 4) && std::strcmp(argv[1], "parse") == 0) {
    const int threads =
        argc == 4 ? static_cast<int>(std::strtol(argv[3], nullptr, 10)) : 1;
    return Parse(argv[2], threads);
  }
  std::fprintf(stderr,
               "usage:\n"
               "  %s generate <triangles> <file>\n"
               "  %s parse <file> [num_threads]\n",
               argv[0], argv[0]);
  return 2;
}
