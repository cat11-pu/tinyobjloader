#!/usr/bin/env python3
"""Generate a wavy-grid OBJ where every face corner owns its own
position/normal/texcoord records (the classic duplicated-vertex layout).

Welding corners that share identical position, normal and texcoord must
collapse the grid back to one vertex per lattice point.
"""

import math
import sys


def main() -> None:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "wavy_grid.obj"
    gnx = int(sys.argv[2]) if len(sys.argv) > 2 else 121
    gnz = int(sys.argv[3]) if len(sys.argv) > 3 else 121
    extent = 2.0

    def height(x: float, z: float) -> float:
        return 0.35 * math.sin(1.8 * x) * math.cos(1.5 * z)

    v_lines = ["# Expanded wavy grid: every face corner owns its own "
               "v/vn/vt records; v/vt/vn share the same 1-based id"]
    vt_lines = []
    vn_lines = []
    face_lines = []
    corner_index = 0

    for iz in range(gnz - 1):
        for ix in range(gnx - 1):
            grid_corners = [(ix, iz), (ix + 1, iz), (ix + 1, iz + 1),
                            (ix, iz + 1)]
            # Two triangles per cell; shared edges keep duplicated records.
            tris = [(0, 1, 2), (0, 2, 3)]
            for tri in tris:
                face = []
                for corner_idx in tri:
                    gx, gz = grid_corners[corner_idx]
                    x = -extent + 2.0 * extent * gx / (gnx - 1)
                    z = -extent + 2.0 * extent * gz / (gnz - 1)
                    y = height(x, z)
                    # Analytic normal of y = h(x, z).
                    dh_dx = 0.35 * 1.8 * math.cos(1.8 * x) * math.cos(1.5 * z)
                    dh_dz = -0.35 * 1.5 * math.sin(1.8 * x) * math.sin(1.5 * z)
                    nx_, ny_, nz_ = -dh_dx, 1.0, -dh_dz
                    length = math.sqrt(nx_ * nx_ + ny_ * ny_ +
                                       nz_ * nz_)
                    nx_, ny_, nz_ = nx_ / length, ny_ / length, nz_ / length
                    u = gx / (gnx - 1)
                    v = gz / (gnz - 1)

                    corner_index += 1
                    face.append(corner_index)  # v/vt/vn share the 1-based id
                    v_lines.append(f"v {x:.6f} {y:.6f} {z:.6f}")
                    vt_lines.append(f"vt {u:.6f} {v:.6f}")
                    vn_lines.append(
                        f"vn {nx_:.6f} {ny_:.6f} {nz_:.6f}")
                # v, vt and vn records are emitted in lockstep, so all
                # three attributes of a corner share the same 1-based id.
                refs = " ".join(f"{i}/{i}/{i}" for i in face)
                face_lines.append(f"f {refs}")

    with open(out_path, "w") as fp:
        fp.write("\n".join(v_lines + vt_lines + vn_lines + face_lines) +
                 "\n")
    print(f"wrote {out_path}: {corner_index} corners, "
          f"{len(face_lines)} triangles")


if __name__ == "__main__":
    main()
