#ifdef MESHMODULE_CLASS
MeshModuleStyle(fabric,MeshModuleFabric)
#else
#ifndef LMP_MESH_MODULE_FABRIC_H
#define LMP_MESH_MODULE_FABRIC_H
#include "mesh_module.h"
#include <array>
#include <string>
#include <vector>
namespace LAMMPS_NS {
class TriMesh;
class TriMeshFabric;
class MeshModuleFabric : public MeshModule {
 public:
  MeshModuleFabric(LAMMPS *, int &, int, char **, FixMeshSurface *);
  void post_create_pre_restart() override;
  void init() override;
  void setup_pre_force(int) override;
  void pre_force(int) override;
  void initial_integrate(int) override;
  void final_integrate() override;
  void add_particle_contribution(int, double *, double *, int, double *) override;
  int setmask() override;
 private:
  struct Node { std::array<double,3> x, v, f; bool pinned; };
  struct Edge { int a, b; double rest, spring, pre_tension, damping; };
  struct Bend { int a, b, c, d; double rest_angle; };
  struct LongitudinalEdge { int a, b; };
  struct SupportTriangle { int index; double lower[3], upper[3]; };
  struct SupportBvhNode { double lower[3], upper[3]; int begin, end, left, right; };
  struct SupportMesh {
    std::string id;
    TriMesh *mesh;
    std::vector<SupportTriangle> triangles;
    std::vector<SupportBvhNode> bvh;
  };
  enum PinBoundary { PIN_NONE, PIN_ALL, PIN_START_END };
  void build_topology();
  void resolve_support_meshes();
  void build_support_bvh(SupportMesh &);
  int build_support_bvh_node(SupportMesh &, int, int);
  bool closest_support_surface(const SupportMesh &, const double *, double *, double *) const;
  void closest_support_surface_node(const SupportMesh &, int, const double *, double &, double *, double *) const;
  void set_material_velocity();
  void calculate_internal_forces();
  void calculate_bending_forces();
  double bending_energy(const Bend &) const;
  void apply_supports(Node &);
  void write_nodes_to_mesh();
  TriMeshFabric *fabric_mesh_;
  std::vector<Node> nodes_;
  std::vector<Edge> edges_;
  std::vector<Bend> bends_;
  std::vector<std::array<int,3> > tri_nodes_;
  std::vector<LongitudinalEdge> longitudinal_edges_;
  std::vector<std::string> support_mesh_ids_;
  std::vector<SupportMesh> support_meshes_;
  double node_mass_, spring_, spring_longitudinal_, spring_transverse_, spring_shear_;
  double damping_, damping_longitudinal_, damping_transverse_, damping_shear_;
  double bending_, pre_tension_, pre_tension_longitudinal_, speed_;
  double direction_[3], gravity_[3], support_z_;
  double cylinder_center_[3], cylinder_radius_;
  int cylinder_axis_;
  bool direction_set_, gravity_set_, support_zplane_, support_cylinder_, initialized_;
  PinBoundary pin_boundary_;
};
}
#endif
#endif
