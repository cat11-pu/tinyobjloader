#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vertexmerge {

// One renderable vertex: position + normal + texture coordinate.
// All fields are float so the bit pattern is stable for hashing.
struct Vertex {
  float px, py, pz;
  float nx, ny, nz;
  float u, v;

  bool operator==(const Vertex& other) const {
    // Exact equality: only bit-identical attributes are merged, which
    // guarantees the rendered image is unchanged.
    return px == other.px && py == other.py && pz == other.pz &&
           nx == other.nx && ny == other.ny && nz == other.nz &&
           u == other.u && v == other.v;
  }
};

struct WeldResult {
  std::vector<Vertex> vertices;  // unique, deduplicated vertex table
  std::vector<uint32_t> indices; // one index per original corner
};

inline uint64_t Fnv1a(const Vertex& key) {
  uint64_t h = 1469598103934665603ull;
  const unsigned char* bytes =
      reinterpret_cast<const unsigned char*>(&key);
  for (size_t i = 0; i < sizeof(Vertex); ++i) {
    h ^= static_cast<uint64_t>(bytes[i]);
    h *= 1099511628211ull;
  }
  return h;
}

// Deduplicate vertices whose position, normal and texcoord are exactly
// identical. Returns a packed vertex table and a remap table with one new
// index for every input corner, so triangle connectivity is preserved.
//
// Complexity: expected O(n) time and O(n) memory using an open-addressing
// hash table with ~50% load factor.
inline WeldResult WeldVertices(const std::vector<Vertex>& corners) {
  WeldResult result;
  const size_t corner_count = corners.size();
  result.indices.resize(corner_count);
  if (corner_count == 0) {
    return result;
  }

  size_t capacity = 16;
  while (capacity < corner_count * 2) {
    capacity <<= 1;
  }
  const uint64_t mask = static_cast<uint64_t>(capacity - 1);
  const uint32_t empty = 0xffffffffu;

  // slot -> index into the unique vertex table (or `empty`).
  std::vector<uint32_t> slot_to_vertex(capacity, empty);
  result.vertices.reserve(corner_count);

  for (size_t corner = 0; corner < corner_count; ++corner) {
    const Vertex& key = corners[corner];
    size_t slot = static_cast<size_t>(Fnv1a(key) & mask);
    while (true) {
      uint32_t existing = slot_to_vertex[slot];
      if (existing == empty) {
        uint32_t new_index =
            static_cast<uint32_t>(result.vertices.size());
        slot_to_vertex[slot] = new_index;
        result.vertices.push_back(key);
        result.indices[corner] = new_index;
        break;
      }
      if (result.vertices[existing] == key) {
        result.indices[corner] = existing;
        break;
      }
      slot = static_cast<size_t>((static_cast<uint64_t>(slot) + 1) &
                                 mask);
    }
  }

  return result;
}

}  // namespace vertexmerge
