#ifndef LMP_TRI_MESH_FABRIC_H
#define LMP_TRI_MESH_FABRIC_H

#include "tri_mesh.h"
#include "vector_liggghts.h"

namespace LAMMPS_NS {
class TriMeshFabric : public TriMesh {
 public:
  TriMeshFabric(LAMMPS *lmp) : TriMesh(lmp) {}
  bool isDeforming() override { return true; }
  void fabric_node(int tri, int node, double *x) { vectorCopy3D(node_(tri)[node], x); }
  void set_fabric_node(int tri, int node, const double *x) { vectorCopy3D(x, node_(tri)[node]); }
  void refresh_fabric_geometry() {
    updateCenterRbound(0, sizeLocal());
    recalcLocalSurfProperties();
    updateGlobalBoundingBox();
  }
};
}
#endif
