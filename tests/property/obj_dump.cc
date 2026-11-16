// obj_dump: parse one OBJ file with tinyobjloader and print a canonical
// dump of the parsed result. Used by the property/differential test runner.
//
// Exit codes:
//   0  parse succeeded (dump on stdout)
//   2  bad usage / cannot open file
//   3  LoadObj reported a parse error (handled failure, NOT a crash)
// Any sanitizer abort (ASan/UBSan/LSan) yields a different exit status and
// is flagged as a crash by the runner.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

static void print_real(double x) { std::printf("%.17g", x); }

static void print_index(const tinyobj::index_t &idx) {
  std::printf("%d/%d/%d", idx.vertex_index, idx.texcoord_index,
              idx.normal_index);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: obj_dump <file.obj>\n");
    return 2;
  }
  std::ifstream ifs(argv[1], std::ios::binary);
  if (!ifs) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  std::stringstream ss;
  ss << ifs.rdbuf();
  const std::string obj_text = ss.str();

  tinyobj::ObjReaderConfig cfg;
  cfg.mtl_search_path = "";
  cfg.triangulate = false;  // keep raw faces for item-by-item comparison
  cfg.vertex_color = true;

  tinyobj::ObjReader reader;
  const bool ok = reader.ParseFromString(obj_text, /*mtl_text=*/"", cfg);

  if (!reader.Warning().empty()) {
    std::fprintf(stderr, "%s", reader.Warning().c_str());
  }
  if (!reader.Error().empty()) {
    std::fprintf(stderr, "%s", reader.Error().c_str());
  }
  if (!ok) {
    return 3;
  }

  const tinyobj::attrib_t &attrib = reader.GetAttrib();
  const std::vector<tinyobj::shape_t> &shapes = reader.GetShapes();

  std::printf("OK\n");

  std::printf("V %zu\n", attrib.vertices.size() / 3);
  for (size_t i = 0; i + 2 < attrib.vertices.size(); i += 3) {
    print_real(attrib.vertices[i]);
    std::printf(" ");
    print_real(attrib.vertices[i + 1]);
    std::printf(" ");
    print_real(attrib.vertices[i + 2]);
    std::printf("\n");
  }

  std::printf("W %zu\n", attrib.vertex_weights.size());
  for (size_t i = 0; i < attrib.vertex_weights.size(); i++) {
    print_real(attrib.vertex_weights[i]);
    std::printf("\n");
  }

  std::printf("VC %zu\n", attrib.colors.size() / 3);
  for (size_t i = 0; i + 2 < attrib.colors.size(); i += 3) {
    print_real(attrib.colors[i]);
    std::printf(" ");
    print_real(attrib.colors[i + 1]);
    std::printf(" ");
    print_real(attrib.colors[i + 2]);
    std::printf("\n");
  }

  std::printf("VT %zu\n", attrib.texcoords.size() / 2);
  for (size_t i = 0; i + 1 < attrib.texcoords.size(); i += 2) {
    print_real(attrib.texcoords[i]);
    std::printf(" ");
    print_real(attrib.texcoords[i + 1]);
    std::printf("\n");
  }

  std::printf("VTW %zu\n", attrib.texcoord_ws.size());
  for (size_t i = 0; i < attrib.texcoord_ws.size(); i++) {
    print_real(attrib.texcoord_ws[i]);
    std::printf("\n");
  }

  std::printf("VN %zu\n", attrib.normals.size() / 3);
  for (size_t i = 0; i + 2 < attrib.normals.size(); i += 3) {
    print_real(attrib.normals[i]);
    std::printf(" ");
    print_real(attrib.normals[i + 1]);
    std::printf(" ");
    print_real(attrib.normals[i + 2]);
    std::printf("\n");
  }

  std::printf("SHAPES %zu\n", shapes.size());
  for (size_t s = 0; s < shapes.size(); s++) {
    const tinyobj::shape_t &shape = shapes[s];
    std::printf("SHAPE %s\n", shape.name.c_str());

    const tinyobj::mesh_t &mesh = shape.mesh;
    std::printf("FACES %zu\n", mesh.num_face_vertices.size());
    size_t base = 0;
    for (size_t f = 0; f < mesh.num_face_vertices.size(); f++) {
      const size_t nv = static_cast<size_t>(mesh.num_face_vertices[f]);
      const int mat =
          (f < mesh.material_ids.size()) ? mesh.material_ids[f] : -2;
      const unsigned int sm =
          (f < mesh.smoothing_group_ids.size()) ? mesh.smoothing_group_ids[f]
                                                : 0;
      std::printf("F %zu %d %u", nv, mat, sm);
      for (size_t k = 0; k < nv; k++) {
        std::printf(" ");
        print_index(mesh.indices[base + k]);
      }
      std::printf("\n");
      base += nv;
    }

    std::printf("LINES %zu\n", shape.lines.num_line_vertices.size());
    base = 0;
    for (size_t l = 0; l < shape.lines.num_line_vertices.size(); l++) {
      const size_t nv = static_cast<size_t>(shape.lines.num_line_vertices[l]);
      std::printf("L %zu", nv);
      for (size_t k = 0; k < nv; k++) {
        std::printf(" ");
        print_index(shape.lines.indices[base + k]);
      }
      std::printf("\n");
      base += nv;
    }

    std::printf("POINTS %zu\n", shape.points.indices.size());
    for (size_t p = 0; p < shape.points.indices.size(); p++) {
      std::printf("P ");
      print_index(shape.points.indices[p]);
      std::printf("\n");
    }

    std::printf("TAGS %zu\n", mesh.tags.size());
    for (size_t t = 0; t < mesh.tags.size(); t++) {
      const tinyobj::tag_t &tag = mesh.tags[t];
      std::printf("T %s I", tag.name.c_str());
      for (size_t i = 0; i < tag.intValues.size(); i++) {
        std::printf(" %d", tag.intValues[i]);
      }
      std::printf(" F");
      for (size_t i = 0; i < tag.floatValues.size(); i++) {
        std::printf(" ");
        print_real(tag.floatValues[i]);
      }
      std::printf(" S");
      for (size_t i = 0; i < tag.stringValues.size(); i++) {
        std::printf(" %s", tag.stringValues[i].c_str());
      }
      std::printf("\n");
    }
  }
  std::printf("END\n");
  return 0;
}
