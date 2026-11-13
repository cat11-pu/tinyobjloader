#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <vector>

#include "weld_vertices.h"

namespace vertexmerge {

// Minimal deterministic software rasterizer. It renders exactly the same
// pixels for an indexed mesh and for its expanded (per-corner) counterpart
// because every per-vertex input value is identical and triangle order,
// tie rules and arithmetic are fixed.

struct Camera {
  float eye[3];
  float target[3];
  float fov_y_radians;
};

struct Image {
  int width;
  int height;
  std::vector<uint8_t> rgb;  // width*height*3, row major
};

namespace detail {

struct Vec3 {
  float x, y, z;
};

inline Vec3 Sub(Vec3 a, Vec3 b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}

inline float Dot(Vec3 a, Vec3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 Normalize(Vec3 a) {
  float inv_len = 1.0f / std::sqrt(Dot(a, a));
  return {a.x * inv_len, a.y * inv_len, a.z * inv_len};
}

struct Frame {
  Vec3 axis_x;
  Vec3 axis_y;
  Vec3 axis_z;  // points from target toward the camera
  Vec3 origin;
  float focal;
};

inline Frame MakeFrame(const Camera& camera, int height) {
  Vec3 eye{camera.eye[0], camera.eye[1], camera.eye[2]};
  Vec3 target{camera.target[0], camera.target[1], camera.target[2]};
  Vec3 forward = Normalize(Sub(target, eye));
  Vec3 world_up{0.0f, 1.0f, 0.0f};
  Vec3 right = Normalize(Cross(forward, world_up));
  Vec3 up = Cross(right, forward);
  float focal = (static_cast<float>(height) * 0.5f) /
                std::tan(camera.fov_y_radians * 0.5f);
  return {right, up, Sub(eye, target), eye, focal};
}

inline void Project(const Frame& frame, Vec3 p, int width, int height,
                    float& sx, float& sy, float& inv_z) {
  Vec3 rel = Sub(p, frame.origin);
  // Camera looks along -axis_z, so depth along the view ray is -rel.axis_z.
  float depth = -Dot(rel, frame.axis_z);
  inv_z = 1.0f / depth;
  float ndc_x = Dot(rel, frame.axis_x) / depth;
  float ndc_y = Dot(rel, frame.axis_y) / depth;
  sx = static_cast<float>(width) * 0.5f +
       ndc_x * frame.focal + 0.5f;
  sy = static_cast<float>(height) * 0.5f -
       ndc_y * frame.focal + 0.5f;
}

}  // namespace detail

// Renders `triangle_count` triangles. Vertex data is fetched through
// `indices` (which for the "before" mesh is simply the identity sequence).
inline Image Render(const std::vector<Vertex>& vertices,
                    const std::vector<uint32_t>& indices,
                    size_t triangle_count, const Camera& camera,
                    int width, int height) {
  using namespace detail;
  Image image;
  image.width = width;
  image.height = height;
  image.rgb.assign(static_cast<size_t>(width) * height * 3, 0);
  std::vector<float> depth_buffer(
      static_cast<size_t>(width) * height,
      -std::numeric_limits<float>::infinity());

  Frame frame = MakeFrame(camera, height);
  const Vec3 light_dir =
      Normalize({-0.45f, 0.8f, 0.4f});

  struct Proj {
    float x, y, inv_z;
    Vec3 normal;
  };

  for (size_t tri = 0; tri < triangle_count; ++tri) {
    Proj proj[3];
    for (int k = 0; k < 3; ++k) {
      const Vertex& v =
          vertices[indices[tri * 3 + static_cast<size_t>(k)]];
      Vec3 p{v.px, v.py, v.pz};
      Project(frame, p, width, height, proj[k].x, proj[k].y,
              proj[k].inv_z);
      proj[k].normal = Normalize({v.nx, v.ny, v.nz});
    }

    // Backface cull in screen space with a fixed winding rule.
    float area2 = (proj[1].x - proj[0].x) * (proj[2].y - proj[0].y) -
                  (proj[1].y - proj[0].y) * (proj[2].x - proj[0].x);
    if (area2 <= 0.0f) {
      continue;
    }

    float min_xf = std::min({proj[0].x, proj[1].x, proj[2].x});
    float max_xf = std::max({proj[0].x, proj[1].x, proj[2].x});
    float min_yf = std::min({proj[0].y, proj[1].y, proj[2].y});
    float max_yf = std::max({proj[0].y, proj[1].y, proj[2].y});
    int min_x = std::max(0, static_cast<int>(std::floor(min_xf)));
    int max_x = std::min(width - 1, static_cast<int>(std::floor(max_xf)));
    int min_y = std::max(0, static_cast<int>(std::floor(min_yf)));
    int max_y =
        std::min(height - 1, static_cast<int>(std::floor(max_yf)));

    float inv_area2 = 1.0f / area2;
    for (int py = min_y; py <= max_y; ++py) {
      for (int px = min_x; px <= max_x; ++px) {
        float sample_x = static_cast<float>(px) + 0.5f;
        float sample_y = static_cast<float>(py) + 0.5f;

        float w0 = ((proj[1].x - sample_x) * (proj[2].y - sample_y) -
                    (proj[1].y - sample_y) * (proj[2].x - sample_x)) *
                   inv_area2;
        float w1 = ((proj[2].x - sample_x) * (proj[0].y - sample_y) -
                    (proj[2].y - sample_y) * (proj[0].x - sample_x)) *
                   inv_area2;
        float w2 = 1.0f - w0 - w1;
        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
          continue;
        }

        // Perspective-correct interpolation of 1/z and the normal.
        float inv_z = w0 * proj[0].inv_z + w1 * proj[1].inv_z +
                      w2 * proj[2].inv_z;
        float perspective = 1.0f / inv_z;
        float n0 = w0 * proj[0].inv_z * perspective;
        float n1 = w1 * proj[1].inv_z * perspective;
        float n2 = w2 * proj[2].inv_z * perspective;
        Vec3 normal{
            n0 * proj[0].normal.x + n1 * proj[1].normal.x +
                n2 * proj[2].normal.x,
            n0 * proj[0].normal.y + n1 * proj[1].normal.y +
                n2 * proj[2].normal.y,
            n0 * proj[0].normal.z + n1 * proj[1].normal.z +
                n2 * proj[2].normal.z};
        normal = Normalize(normal);

        size_t pixel = static_cast<size_t>(py) * width + px;
        if (inv_z <= depth_buffer[pixel]) {
          continue;
        }
        depth_buffer[pixel] = inv_z;

        float diffuse = std::max(0.0f, Dot(normal, light_dir));
        float intensity = 0.18f + 0.82f * diffuse;
        auto shade = [intensity](float base) -> uint8_t {
          float value = base * intensity;
          int q = static_cast<int>(std::lround(value * 255.0f));
          return static_cast<uint8_t>(std::clamp(q, 0, 255));
        };
        uint8_t* out = &image.rgb[pixel * 3];
        out[0] = shade(0.82f);
        out[1] = shade(0.45f);
        out[2] = shade(0.28f);
      }
    }
  }

  return image;
}

inline bool WriteBinaryPpm(const Image& image, const char* path) {
  FILE* fp = std::fopen(path, "wb");
  if (!fp) {
    return false;
  }
  std::fprintf(fp, "P6\n%d %d\n255\n", image.width, image.height);
  std::fwrite(image.rgb.data(), 1, image.rgb.size(), fp);
  bool ok = std::ferror(fp) == 0;
  std::fclose(fp);
  return ok;
}

}  // namespace vertexmerge
