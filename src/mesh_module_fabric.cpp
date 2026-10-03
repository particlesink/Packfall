#include "mesh_module_fabric.h"
#include "atom.h"
#include "comm.h"
#include "error.h"
#include "fix_mesh_surface.h"
#include "force.h"
#include "math_extra_liggghts.h"
#include "modify.h"
#include "neighbor.h"
#include "tri_mesh_fabric.h"
#include "tri_mesh.h"
#include "update.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>

using namespace LAMMPS_NS;
using namespace FixConst;
namespace {
struct VertexBin {
  long long x, y, z;
  bool operator<(const VertexBin &other) const {
    if (x != other.x) return x < other.x;
    if (y != other.y) return y < other.y;
    return z < other.z;
  }
};
VertexBin vertex_bin(const double *x, const double tolerance) {
  return VertexBin{static_cast<long long>(std::llround(x[0]/tolerance)),
                   static_cast<long long>(std::llround(x[1]/tolerance)),
                   static_cast<long long>(std::llround(x[2]/tolerance))};
}
double magnitude(const std::array<double,3> &v) {
  return std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}
double dihedral_angle(const std::array<double,3> &a, const std::array<double,3> &b,
                      const std::array<double,3> &c, const std::array<double,3> &d) {
  const std::array<double,3> edge={{b[0]-a[0],b[1]-a[1],b[2]-a[2]}};
  const std::array<double,3> ac={{c[0]-a[0],c[1]-a[1],c[2]-a[2]}};
  const std::array<double,3> ad={{d[0]-a[0],d[1]-a[1],d[2]-a[2]}};
  std::array<double,3> n0={{edge[1]*ac[2]-edge[2]*ac[1], edge[2]*ac[0]-edge[0]*ac[2], edge[0]*ac[1]-edge[1]*ac[0]}};
  std::array<double,3> n1={{ad[1]*edge[2]-ad[2]*edge[1], ad[2]*edge[0]-ad[0]*edge[2], ad[0]*edge[1]-ad[1]*edge[0]}};
  const double l0=magnitude(n0), l1=magnitude(n1);
  if (l0 < 1.e-20 || l1 < 1.e-20) return 0.;
  double cosine=(n0[0]*n1[0]+n0[1]*n1[1]+n0[2]*n1[2])/(l0*l1);
  cosine=std::max(-1.,std::min(1.,cosine));
  return std::acos(cosine);
}
int axis_index(const char *axis) {
  if (!strcmp(axis,"x")) return 0;
  if (!strcmp(axis,"y")) return 1;
  if (!strcmp(axis,"z")) return 2;
  return -1;
}
bool normal_projection_on_triangle(const double *p, const double *a, const double *b,
                                   const double *c, const double *normal, double *projected) {
  const double signed_distance=(p[0]-a[0])*normal[0]+(p[1]-a[1])*normal[1]+(p[2]-a[2])*normal[2];
  for (int d=0; d<3; ++d) projected[d]=p[d]-signed_distance*normal[d];
  const double *vertices[3]={a,b,c};
  for (int i=0; i<3; ++i) {
    const double *u=vertices[i], *v=vertices[(i+1)%3];
    const double edge[3]={v[0]-u[0],v[1]-u[1],v[2]-u[2]};
    const double offset[3]={projected[0]-u[0],projected[1]-u[1],projected[2]-u[2]};
    const double cross[3]={edge[1]*offset[2]-edge[2]*offset[1],
                           edge[2]*offset[0]-edge[0]*offset[2],
                           edge[0]*offset[1]-edge[1]*offset[0]};
    if (cross[0]*normal[0]+cross[1]*normal[1]+cross[2]*normal[2] < -1.e-12) return false;
  }
  return true;
}
}
double point_to_aabb_distance_squared(const double *point, const double *lower, const double *upper) {
  double distance_squared=0.;
  for (int d=0; d<3; ++d) {
    const double delta=point[d] < lower[d] ? lower[d]-point[d] : (point[d] > upper[d] ? point[d]-upper[d] : 0.);
    distance_squared+=delta*delta;
  }
  return distance_squared;
}
struct Dual12 {
  double value;
  std::array<double,12> derivative;
  explicit Dual12(const double initial=0.) : value(initial) { derivative.fill(0.); }
};
struct DualVector3 { Dual12 value[3]; };
Dual12 dual_add(const Dual12 &a, const Dual12 &b) {
  Dual12 result(a.value+b.value);
  for (int i=0; i<12; ++i) result.derivative[i]=a.derivative[i]+b.derivative[i];
  return result;
}
Dual12 dual_subtract(const Dual12 &a, const Dual12 &b) {
  Dual12 result(a.value-b.value);
  for (int i=0; i<12; ++i) result.derivative[i]=a.derivative[i]-b.derivative[i];
  return result;
}
Dual12 dual_multiply(const Dual12 &a, const Dual12 &b) {
  Dual12 result(a.value*b.value);
  for (int i=0; i<12; ++i) result.derivative[i]=a.derivative[i]*b.value+a.value*b.derivative[i];
  return result;
}
Dual12 dual_divide(const Dual12 &a, const Dual12 &b) {
  Dual12 result(a.value/b.value);
  const double inverse=1./(b.value*b.value);
  for (int i=0; i<12; ++i) result.derivative[i]=(a.derivative[i]*b.value-a.value*b.derivative[i])*inverse;
  return result;
}
Dual12 dual_sqrt(const Dual12 &a) {
  Dual12 result(std::sqrt(a.value));
  const double scale=.5/result.value;
  for (int i=0; i<12; ++i) result.derivative[i]=scale*a.derivative[i];
  return result;
}
Dual12 dual_acos(const Dual12 &a) {
  Dual12 result(std::acos(a.value));
  const double scale=-1./std::sqrt(1.-a.value*a.value);
  for (int i=0; i<12; ++i) result.derivative[i]=scale*a.derivative[i];
  return result;
}
DualVector3 dual_subtract(const DualVector3 &a, const DualVector3 &b) {
  DualVector3 result;
  for (int d=0; d<3; ++d) result.value[d]=dual_subtract(a.value[d],b.value[d]);
  return result;
}
DualVector3 dual_cross(const DualVector3 &a, const DualVector3 &b) {
  DualVector3 result;
  result.value[0]=dual_subtract(dual_multiply(a.value[1],b.value[2]),dual_multiply(a.value[2],b.value[1]));
  result.value[1]=dual_subtract(dual_multiply(a.value[2],b.value[0]),dual_multiply(a.value[0],b.value[2]));
  result.value[2]=dual_subtract(dual_multiply(a.value[0],b.value[1]),dual_multiply(a.value[1],b.value[0]));
  return result;
}
Dual12 dual_dot(const DualVector3 &a, const DualVector3 &b) {
  return dual_add(dual_add(dual_multiply(a.value[0],b.value[0]),dual_multiply(a.value[1],b.value[1])),dual_multiply(a.value[2],b.value[2]));
}
bool dihedral_angle_gradient(const std::array<double,3> &a, const std::array<double,3> &b,
                             const std::array<double,3> &c, const std::array<double,3> &d,
                             double &angle, double *gradient) {
  const std::array<double,3> *coordinates[4]={&a,&b,&c,&d};
  DualVector3 point[4];
  for (int vertex=0; vertex<4; ++vertex) for (int component=0; component<3; ++component) {
    point[vertex].value[component]=Dual12((*coordinates[vertex])[component]);
    point[vertex].value[component].derivative[3*vertex+component]=1.;
  }
  const DualVector3 edge=dual_subtract(point[1],point[0]);
  const DualVector3 ac=dual_subtract(point[2],point[0]);
  const DualVector3 ad=dual_subtract(point[3],point[0]);
  const DualVector3 normal0=dual_cross(edge,ac);
  const DualVector3 normal1=dual_cross(ad,edge);
  const Dual12 length0=dual_sqrt(dual_dot(normal0,normal0));
  const Dual12 length1=dual_sqrt(dual_dot(normal1,normal1));
  if (length0.value < 1.e-20 || length1.value < 1.e-20) return false;
  Dual12 cosine=dual_divide(dual_dot(normal0,normal1),dual_multiply(length0,length1));
  if (cosine.value <= -1. || cosine.value >= 1.) {
    angle=std::acos(std::max(-1.,std::min(1.,cosine.value)));
    for (int i=0; i<12; ++i) gradient[i]=0.;
    return true;
  }
  const Dual12 result=dual_acos(cosine);
  angle=result.value;
  for (int i=0; i<12; ++i) gradient[i]=result.derivative[i];
  return true;
}

MeshModuleFabric::MeshModuleFabric(LAMMPS *lmp, int &iarg, int narg, char **arg,
                                   FixMeshSurface *fix_mesh)
 : MeshModule(lmp,iarg,narg,arg,fix_mesh), fabric_mesh_(dynamic_cast<TriMeshFabric*>(mesh)),
   node_mass_(-1.), spring_(-1.), spring_longitudinal_(-1.), spring_transverse_(-1.), spring_shear_(-1.),
   damping_(0.), damping_longitudinal_(-1.), damping_transverse_(-1.), damping_shear_(-1.),
   bending_(0.), pre_tension_(0.), pre_tension_longitudinal_(0.), speed_(0.),
   direction_{0.,0.,0.}, gravity_{0.,0.,0.}, support_z_(0.),
   cylinder_center_{0.,0.,0.}, cylinder_radius_(0.), cylinder_axis_(-1),
   direction_set_(false), gravity_set_(false), support_zplane_(false),
   support_cylinder_(false), initialized_(false), pin_boundary_(PIN_ALL) {
  if (!fabric_mesh_) error->all(FLERR,"Internal error: fabric module requires TriMeshFabric");
  bool more = true;
  while (iarg < narg && more) {
    more = false;
    if (!strcmp(arg[iarg],"node_mass")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric node_mass requires a value");
      node_mass_=force->numeric(FLERR,arg[++iarg]); ++iarg; more=true;
    } else if (!strcmp(arg[iarg],"spring")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric spring requires a value");
      spring_=force->numeric(FLERR,arg[++iarg]); ++iarg; more=true;
    } else if (!strcmp(arg[iarg],"spring_longitudinal")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric spring_longitudinal requires a value");
      spring_longitudinal_=force->numeric(FLERR,arg[++iarg]); ++iarg; more=true;
    } else if (!strcmp(arg[iarg],"spring_transverse")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric spring_transverse requires a value");
      spring_transverse_=force->numeric(FLERR,arg[++iarg]); ++iarg; more=true;
    } else if (!strcmp(arg[iarg],"spring_shear")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric spring_shear requires a value");
      spring_shear_=force->numeric(FLERR,arg[++iarg]); ++iarg; more=true;
    } else if (!strcmp(arg[iarg],"damping")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric damping requires a value");
      damping_=force->numeric(FLERR,arg[++iarg]); ++iarg; more=true;
    } else if (!strcmp(arg[iarg],"damping_longitudinal")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric damping_longitudinal requires a value");
      damping_longitudinal_=force->numeric(FLERR,arg[++iarg]); ++iarg; more=true;
    } else if (!strcmp(arg[iarg],"damping_transverse")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric damping_transverse requires a value");
      damping_transverse_=force->numeric(FLERR,arg[++iarg]); ++iarg; more=true;
    } else if (!strcmp(arg[iarg],"damping_shear")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric damping_shear requires a value");
      damping_shear_=force->numeric(FLERR,arg[++iarg]); ++iarg; more=true;
    } else if (!strcmp(arg[iarg],"bending")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric bending requires a value");
      bending_=force->numeric(FLERR,arg[++iarg]); ++iarg; more=true;
    } else if (!strcmp(arg[iarg],"pre_tension")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric pre_tension requires a value");
      pre_tension_=force->numeric(FLERR,arg[++iarg]); ++iarg; more=true;
    } else if (!strcmp(arg[iarg],"pre_tension_longitudinal")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric pre_tension_longitudinal requires a value");
      pre_tension_longitudinal_=force->numeric(FLERR,arg[++iarg]); ++iarg; more=true;
    } else if (!strcmp(arg[iarg],"speed")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric speed requires a value");
      speed_=force->numeric(FLERR,arg[++iarg]); ++iarg; more=true;
    } else if (!strcmp(arg[iarg],"transport_direction")) {
      if (iarg+3 >= narg) error->all(FLERR,"Fabric transport_direction requires three values");
      for (int d=0; d<3; ++d) direction_[d]=force->numeric(FLERR,arg[++iarg]);
      ++iarg; direction_set_=true; more=true;
    } else if (!strcmp(arg[iarg],"gravity")) {
      if (iarg+3 >= narg) error->all(FLERR,"Fabric gravity requires three values");
      for (int d=0; d<3; ++d) gravity_[d]=force->numeric(FLERR,arg[++iarg]);
      ++iarg; gravity_set_=true; more=true;
    } else if (!strcmp(arg[iarg],"support_zplane")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric support_zplane requires a value");
      support_z_=force->numeric(FLERR,arg[++iarg]); ++iarg; support_zplane_=true; more=true;
    } else if (!strcmp(arg[iarg],"support_cylinder")) {
      if (iarg+7 >= narg) error->all(FLERR,"Fabric support_cylinder requires: axis x|y|z center x y z radius r");
      cylinder_axis_=axis_index(arg[++iarg]);
      if (cylinder_axis_ < 0) error->all(FLERR,"Fabric support_cylinder axis must be x, y, or z");
      if (strcmp(arg[++iarg],"center")) error->all(FLERR,"Fabric support_cylinder expects keyword center");
      for (int d=0; d<3; ++d) cylinder_center_[d]=force->numeric(FLERR,arg[++iarg]);
      if (strcmp(arg[++iarg],"radius")) error->all(FLERR,"Fabric support_cylinder expects keyword radius");
      cylinder_radius_=force->numeric(FLERR,arg[++iarg]);
      ++iarg; support_cylinder_=true; more=true;
    } else if (!strcmp(arg[iarg],"support_mesh")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric support_mesh requires a mesh/surface fix ID");
      support_mesh_ids_.push_back(arg[++iarg]);
      ++iarg; more=true;
    } else if (!strcmp(arg[iarg],"pin_boundary")) {
      if (iarg+1 >= narg) error->all(FLERR,"Fabric pin_boundary requires all, none, start_end, yes, or no");
      const char *value=arg[++iarg];
      if (!strcmp(value,"yes") || !strcmp(value,"all")) pin_boundary_=PIN_ALL;
      else if (!strcmp(value,"no") || !strcmp(value,"none")) pin_boundary_=PIN_NONE;
      else if (!strcmp(value,"start_end")) pin_boundary_=PIN_START_END;
      else error->all(FLERR,"Fabric pin_boundary expects all, none, start_end, yes, or no");
      ++iarg; more=true;
    }
  }
}

void MeshModuleFabric::post_create_pre_restart() {
  if (fix_mesh->surfaceVel()) error->all(FLERR,"mesh/surface/fabric owns material velocity; do not use surface_vel");
  mesh->prop().addElementProperty<MultiVectorContainer<double,3,3> >(
    "v","comm_exchange_borders","frame_invariant","restart_no");
}

void MeshModuleFabric::init() {
  if (comm->nprocs != 1) error->all(FLERR,"mesh/surface/fabric is currently serial-only");
  if (node_mass_ <= 0.) error->all(FLERR,"Fabric node_mass must be > 0");
  const bool directional_spring=spring_longitudinal_ >= 0. || spring_transverse_ >= 0. || spring_shear_ >= 0.;
  if (spring_ <= 0. && !directional_spring) error->all(FLERR,"Fabric spring must be > 0");
  if (spring_longitudinal_ < 0.) spring_longitudinal_=spring_;
  if (spring_transverse_ < 0.) spring_transverse_=spring_;
  if (spring_shear_ < 0.) spring_shear_=spring_;
  if (spring_longitudinal_ <= 0. || spring_transverse_ <= 0. || spring_shear_ <= 0.)
    error->all(FLERR,"Fabric directional spring stiffnesses must be > 0 (provide spring or all directional values)");
  const bool directional_damping=damping_longitudinal_ >= 0. || damping_transverse_ >= 0. || damping_shear_ >= 0.;
  if (damping_ < 0.) error->all(FLERR,"Fabric damping must be >= 0");
  if (damping_longitudinal_ < 0.) damping_longitudinal_=damping_;
  if (damping_transverse_ < 0.) damping_transverse_=damping_;
  if (damping_shear_ < 0.) damping_shear_=damping_;
  if (damping_longitudinal_ < 0. || damping_transverse_ < 0. || damping_shear_ < 0.)
    error->all(FLERR,"Fabric directional damping values must be >= 0");
  if (bending_ < 0.) error->all(FLERR,"Fabric bending must be >= 0");
  if (pre_tension_ < 0. || pre_tension_longitudinal_ < 0.) error->all(FLERR,"Fabric pre-tensions must be >= 0");
  const double direction_length=std::sqrt(direction_[0]*direction_[0]+direction_[1]*direction_[1]+direction_[2]*direction_[2]);
  if ((speed_ != 0. || directional_spring || directional_damping || pre_tension_longitudinal_ != 0. || pin_boundary_ == PIN_START_END) &&
      (!direction_set_ || direction_length < 1.e-12))
    error->all(FLERR,"Fabric directional stiffness, tension, or speed requires a non-zero transport_direction");
  if (support_cylinder_ && cylinder_radius_ <= 0.) error->all(FLERR,"Fabric support_cylinder radius must be > 0");
  resolve_support_meshes();
}

void MeshModuleFabric::resolve_support_meshes() {
  support_meshes_.clear();
  for (std::vector<std::string>::const_iterator id=support_mesh_ids_.begin(); id!=support_mesh_ids_.end(); ++id) {
    FixMeshSurface *support=dynamic_cast<FixMeshSurface*>(modify->find_fix_id(id->c_str()));
    if (!support) error->all(FLERR,"Fabric support_mesh must name an existing mesh/surface fix defined before the fabric fix");
    if (support == fix_mesh) error->all(FLERR,"Fabric cannot use itself as a support_mesh");
    TriMesh *support_mesh=support->triMesh();
    if (support_mesh->sizeLocal() == 0) error->all(FLERR,"Fabric support_mesh contains no triangles");
    SupportMesh entry;
    entry.id=*id;
    entry.mesh=support_mesh;
    build_support_bvh(entry);
    support_meshes_.push_back(entry);
  }
}

void MeshModuleFabric::build_support_bvh(SupportMesh &support) {
  support.triangles.clear();
  support.bvh.clear();
  if (support.mesh->sizeLocal() <= 128) return;
  for (int i=0; i<support.mesh->sizeLocal(); ++i) {
    SupportTriangle triangle;
    triangle.index=i;
    for (int d=0; d<3; ++d) triangle.lower[d]=triangle.upper[d]=0.;
    for (int vertex=0; vertex<3; ++vertex) {
      double x[3];
      support.mesh->node(i,vertex,x);
      for (int d=0; d<3; ++d) {
        if (vertex == 0) triangle.lower[d]=triangle.upper[d]=x[d];
        else { triangle.lower[d]=std::min(triangle.lower[d],x[d]); triangle.upper[d]=std::max(triangle.upper[d],x[d]); }
      }
    }
    support.triangles.push_back(triangle);
  }
  if (!support.triangles.empty()) build_support_bvh_node(support,0,support.triangles.size());
}

int MeshModuleFabric::build_support_bvh_node(SupportMesh &support, int begin, int end) {
  SupportBvhNode node;
  for (int d=0; d<3; ++d) node.lower[d]=1.e300, node.upper[d]=-1.e300;
  node.begin=begin; node.end=end; node.left=node.right=-1;
  for (int i=begin; i<end; ++i) for (int d=0; d<3; ++d) {
    node.lower[d]=std::min(node.lower[d],support.triangles[i].lower[d]);
    node.upper[d]=std::max(node.upper[d],support.triangles[i].upper[d]);
  }
  const int index=support.bvh.size();
  support.bvh.push_back(node);
  if (end-begin <= 32) return index;
  int axis=0;
  if (node.upper[1]-node.lower[1] > node.upper[axis]-node.lower[axis]) axis=1;
  if (node.upper[2]-node.lower[2] > node.upper[axis]-node.lower[axis]) axis=2;
  const int middle=(begin+end)/2;
  std::nth_element(support.triangles.begin()+begin,support.triangles.begin()+middle,support.triangles.begin()+end,
    [axis](const SupportTriangle &a, const SupportTriangle &b) {
      return a.lower[axis]+a.upper[axis] < b.lower[axis]+b.upper[axis];
    });
  const int left=build_support_bvh_node(support,begin,middle);
  const int right=build_support_bvh_node(support,middle,end);
  support.bvh[index].left=left;
  support.bvh[index].right=right;
  return index;
}

bool MeshModuleFabric::closest_support_surface(const SupportMesh &support, const double *point, double *closest, double *normal) const {
  double distance_squared=1.e300;
  if (support.bvh.empty()) {
    for (int triangle=0; triangle<support.mesh->sizeLocal(); ++triangle) {
      double a[3], b[3], c[3], candidate[3], triangle_normal[3];
      support.mesh->node(triangle,0,a); support.mesh->node(triangle,1,b); support.mesh->node(triangle,2,c);
      support.mesh->surfaceNorm(triangle,triangle_normal);
      if (!normal_projection_on_triangle(point,a,b,c,triangle_normal,candidate)) continue;
      const double dx=point[0]-candidate[0], dy=point[1]-candidate[1], dz=point[2]-candidate[2];
      const double candidate_distance_squared=dx*dx+dy*dy+dz*dz;
      if (candidate_distance_squared < distance_squared) {
        distance_squared=candidate_distance_squared;
        for (int d=0; d<3; ++d) { closest[d]=candidate[d]; normal[d]=triangle_normal[d]; }
      }
    }
  } else {
    closest_support_surface_node(support,0,point,distance_squared,closest,normal);
  }
  return distance_squared < 1.e299;
}

void MeshModuleFabric::closest_support_surface_node(const SupportMesh &support, int index, const double *point, double &distance_squared, double *closest, double *normal) const {
  const SupportBvhNode &node=support.bvh[index];
  if (point_to_aabb_distance_squared(point,node.lower,node.upper) > distance_squared) return;
  if (node.left < 0) {
    for (int it=node.begin; it<node.end; ++it) {
      const int triangle=support.triangles[it].index;
      double a[3], b[3], c[3], candidate[3], triangle_normal[3];
      support.mesh->node(triangle,0,a); support.mesh->node(triangle,1,b); support.mesh->node(triangle,2,c);
      support.mesh->surfaceNorm(triangle,triangle_normal);
      if (!normal_projection_on_triangle(point,a,b,c,triangle_normal,candidate)) continue;
      const double dx=point[0]-candidate[0], dy=point[1]-candidate[1], dz=point[2]-candidate[2];
      const double candidate_distance_squared=dx*dx+dy*dy+dz*dz;
      if (candidate_distance_squared < distance_squared) {
        distance_squared=candidate_distance_squared;
        for (int d=0; d<3; ++d) { closest[d]=candidate[d]; normal[d]=triangle_normal[d]; }
      }
    }
    return;
  }
  const SupportBvhNode &left=support.bvh[node.left], &right=support.bvh[node.right];
  if (point_to_aabb_distance_squared(point,left.lower,left.upper) < point_to_aabb_distance_squared(point,right.lower,right.upper)) {
    closest_support_surface_node(support,node.left,point,distance_squared,closest,normal);
    closest_support_surface_node(support,node.right,point,distance_squared,closest,normal);
  } else {
    closest_support_surface_node(support,node.right,point,distance_squared,closest,normal);
    closest_support_surface_node(support,node.left,point,distance_squared,closest,normal);
  }
}

int MeshModuleFabric::setmask() { return INITIAL_INTEGRATE|PRE_FORCE|FINAL_INTEGRATE; }

void MeshModuleFabric::setup_pre_force(int) {
  if (initialized_) return;
  build_topology();
  calculate_internal_forces();
  set_material_velocity();
  initialized_=true;
}

void MeshModuleFabric::build_topology() {
  const int ntri=fabric_mesh_->sizeLocal();
  nodes_.clear(); edges_.clear(); bends_.clear();
  tri_nodes_.assign(ntri,std::array<int,3>{{-1,-1,-1}});
  double lower[3]={1.e300,1.e300,1.e300}, upper[3]={-1.e300,-1.e300,-1.e300};
  for (int i=0; i<ntri; ++i) for (int j=0; j<3; ++j) {
    double x[3]; fabric_mesh_->fabric_node(i,j,x);
    for (int d=0; d<3; ++d) {
      lower[d]=std::min(lower[d],x[d]);
      upper[d]=std::max(upper[d],x[d]);
    }
  }
  const double extent=std::max(upper[0]-lower[0],std::max(upper[1]-lower[1],upper[2]-lower[2]));
  const double node_tolerance=std::max(1.e-12,extent*1.e-9);
  const double node_tolerance_squared=node_tolerance*node_tolerance;
  std::map<VertexBin,std::vector<int> > positions;
  std::map<std::pair<int,int>,std::vector<int> > edge_opposites;
  for (int i=0;i<ntri;++i) {
    for (int j=0;j<3;++j) {
      double x[3]; fabric_mesh_->fabric_node(i,j,x);
      const VertexBin bin=vertex_bin(x,node_tolerance);
      int node=-1;
      for (int dx=-1; dx<=1 && node<0; ++dx)
        for (int dy=-1; dy<=1 && node<0; ++dy)
          for (int dz=-1; dz<=1 && node<0; ++dz) {
            const VertexBin nearby={bin.x+dx,bin.y+dy,bin.z+dz};
            std::map<VertexBin,std::vector<int> >::const_iterator found=positions.find(nearby);
            if (found==positions.end()) continue;
            for (std::vector<int>::const_iterator candidate=found->second.begin(); candidate!=found->second.end(); ++candidate) {
              const Node &state=nodes_[*candidate];
              const double ddx=state.x[0]-x[0], ddy=state.x[1]-x[1], ddz=state.x[2]-x[2];
              if (ddx*ddx+ddy*ddy+ddz*ddz <= node_tolerance_squared) {
                node=*candidate;
                break;
              }
            }
          }
      if (node<0) {
        node=nodes_.size();
        Node state;
        state.x={{x[0],x[1],x[2]}};
        state.v={{0.,0.,0.}};
        state.f={{0.,0.,0.}};
        state.pinned=false;
        nodes_.push_back(state);
        positions[bin].push_back(node);
      }
      tri_nodes_[i][j]=node;
    }
    for (int j=0;j<3;++j) {
      int a=tri_nodes_[i][j], b=tri_nodes_[i][(j+1)%3];
      const int opposite=tri_nodes_[i][(j+2)%3];
      if(a>b) std::swap(a,b);
      edge_opposites[std::make_pair(a,b)].push_back(opposite);
    }
  }
  double longitudinal_min=0., longitudinal_max=0., longitudinal_tolerance=0.;
  double direction_length=0.;
  if (pin_boundary_ == PIN_START_END) {
    direction_length=std::sqrt(direction_[0]*direction_[0]+direction_[1]*direction_[1]+direction_[2]*direction_[2]);
    longitudinal_min=1.e300; longitudinal_max=-1.e300;
    for (std::vector<Node>::const_iterator node=nodes_.begin(); node!=nodes_.end(); ++node) {
      const double coordinate=(node->x[0]*direction_[0]+node->x[1]*direction_[1]+node->x[2]*direction_[2])/direction_length;
      longitudinal_min=std::min(longitudinal_min,coordinate);
      longitudinal_max=std::max(longitudinal_max,coordinate);
    }
    longitudinal_tolerance=std::max(1.e-12,extent*1.e-9);
  }
  for (std::map<std::pair<int,int>,std::vector<int> >::const_iterator it=edge_opposites.begin();it!=edge_opposites.end();++it) {
    const int a=it->first.first, b=it->first.second;
    const std::vector<int> &opposites=it->second;
    const double dx=nodes_[b].x[0]-nodes_[a].x[0];
    const double dy=nodes_[b].x[1]-nodes_[a].x[1];
    const double dz=nodes_[b].x[2]-nodes_[a].x[2];
    const double rest=std::sqrt(dx*dx+dy*dy+dz*dz);
    double spring=spring_;
    double tension=pre_tension_;
    double damping=damping_;
    if (direction_set_) {
      const double direction_length=std::sqrt(direction_[0]*direction_[0]+direction_[1]*direction_[1]+direction_[2]*direction_[2]);
      const double alignment=std::fabs((dx*direction_[0]+dy*direction_[1]+dz*direction_[2])/(rest*direction_length));
      if (alignment > 0.95) {
        spring=spring_longitudinal_;
        tension+=pre_tension_longitudinal_;
        damping=damping_longitudinal_;
      } else if (alignment < 0.20) {
        spring=spring_transverse_;
        damping=damping_transverse_;
      } else {
        spring=spring_shear_;
        damping=damping_shear_;
      }
    }
    edges_.push_back(Edge{a,b,rest,spring,tension,damping});
    if (opposites.size()==1) {
      if (pin_boundary_ == PIN_ALL) nodes_[a].pinned=nodes_[b].pinned=true;
      else if (pin_boundary_ == PIN_START_END) {
        const double coordinate_a=(nodes_[a].x[0]*direction_[0]+nodes_[a].x[1]*direction_[1]+nodes_[a].x[2]*direction_[2])/direction_length;
        const double coordinate_b=(nodes_[b].x[0]*direction_[0]+nodes_[b].x[1]*direction_[1]+nodes_[b].x[2]*direction_[2])/direction_length;
        if ((std::fabs(coordinate_a-longitudinal_min) <= longitudinal_tolerance && std::fabs(coordinate_b-longitudinal_min) <= longitudinal_tolerance) ||
            (std::fabs(coordinate_a-longitudinal_max) <= longitudinal_tolerance && std::fabs(coordinate_b-longitudinal_max) <= longitudinal_tolerance))
          nodes_[a].pinned=nodes_[b].pinned=true;
      }
    } else if (opposites.size()==2) {
      bends_.push_back(Bend{a,b,opposites[0],opposites[1],
        dihedral_angle(nodes_[a].x,nodes_[b].x,nodes_[opposites[0]].x,nodes_[opposites[1]].x)});
    } else error->all(FLERR,"Fabric mesh must not contain non-manifold edges");
  }
  if(nodes_.empty()||edges_.empty()) error->all(FLERR,"Fabric mesh needs non-degenerate triangles");

  longitudinal_edges_.assign(ntri,LongitudinalEdge{-1,-1});
  if (!direction_set_) return;
  for (int i=0; i<ntri; ++i) {
    double best_projection=-1.;
    LongitudinalEdge best={-1,-1};
    for (int j=0; j<3; ++j) {
      int a=tri_nodes_[i][j], b=tri_nodes_[i][(j+1)%3];
      const double projection=(nodes_[b].x[0]-nodes_[a].x[0])*direction_[0]
                            +(nodes_[b].x[1]-nodes_[a].x[1])*direction_[1]
                            +(nodes_[b].x[2]-nodes_[a].x[2])*direction_[2];
      if (std::fabs(projection)>best_projection) {
        if (projection < 0.) std::swap(a,b);
        best={a,b};
        best_projection=std::fabs(projection);
      }
    }
    if (speed_ != 0. && best_projection < 1.e-12)
      error->all(FLERR,"Fabric transport_direction must align with an edge in every triangle");
    longitudinal_edges_[i]=best;
  }
}

void MeshModuleFabric::set_material_velocity() {
  MultiVectorContainer<double,3,3> *property=
    mesh->prop().getElementProperty<MultiVectorContainer<double,3,3> >("v");
  double ***velocity=property->begin();
  for(int i=0;i<fabric_mesh_->sizeLocal();++i) {
    double tangent[3]={0.,0.,0.};
    if (speed_ != 0.) {
      const LongitudinalEdge edge=longitudinal_edges_[i];
      for (int d=0; d<3; ++d) tangent[d]=nodes_[edge.b].x[d]-nodes_[edge.a].x[d];
      const double length=std::sqrt(tangent[0]*tangent[0]+tangent[1]*tangent[1]+tangent[2]*tangent[2]);
      if (length < 1.e-12) error->all(FLERR,"Fabric longitudinal edge collapsed to zero length");
      for(int d=0;d<3;++d) tangent[d]*=speed_/length;
    }
    for(int n=0;n<3;++n) for(int d=0;d<3;++d) velocity[i][n][d]=tangent[d];
  }
}

void MeshModuleFabric::pre_force(int) {
  for(std::vector<Node>::iterator it=nodes_.begin();it!=nodes_.end();++it) {
    it->f={{0.,0.,0.}};
    if (gravity_set_) for (int d=0; d<3; ++d) it->f[d]+=node_mass_*gravity_[d];
  }
}

void MeshModuleFabric::apply_supports(Node &node) {
  if (support_zplane_ && node.x[2] < support_z_) {
    node.x[2]=support_z_;
    if (node.v[2] < 0.) node.v[2]=0.;
  }
  if (support_cylinder_) {
    double normal[3]={0.,0.,0.};
    double radial_squared=0.;
    for (int d=0; d<3; ++d) if (d != cylinder_axis_) {
      normal[d]=node.x[d]-cylinder_center_[d];
      radial_squared+=normal[d]*normal[d];
    }
    const double radial=std::sqrt(radial_squared);
    if (radial < cylinder_radius_) {
      if (radial < 1.e-14) error->all(FLERR,"Fabric node reached the support_cylinder axis");
      for (int d=0; d<3; ++d) if (d != cylinder_axis_) normal[d]/=radial;
      for (int d=0; d<3; ++d) if (d != cylinder_axis_)
        node.x[d]=cylinder_center_[d]+cylinder_radius_*normal[d];
      double radial_velocity=0.;
      for (int d=0; d<3; ++d) radial_velocity+=node.v[d]*normal[d];
      if (radial_velocity < 0.) for (int d=0; d<3; ++d) node.v[d]-=radial_velocity*normal[d];
    }
  }

  double deepest=0., contact[3]={0.,0.,0.}, contact_normal[3]={0.,0.,0.};
  for (std::vector<SupportMesh>::const_iterator support=support_meshes_.begin(); support!=support_meshes_.end(); ++support) {
    double closest[3]={0.,0.,0.}, normal[3]={0.,0.,0.};
    if (!closest_support_surface(*support,node.x.data(),closest,normal)) continue;
    const double signed_distance=(node.x[0]-closest[0])*normal[0]
                               +(node.x[1]-closest[1])*normal[1]
                               +(node.x[2]-closest[2])*normal[2];
    if (-signed_distance > deepest) {
      deepest=-signed_distance;
      for (int d=0;d<3;++d) { contact[d]=closest[d]; contact_normal[d]=normal[d]; }
    }
  }
  if (deepest <= 0.) return;
  for (int d=0;d<3;++d) node.x[d]=contact[d];
  const double normal_velocity=node.v[0]*contact_normal[0]+node.v[1]*contact_normal[1]+node.v[2]*contact_normal[2];
  if (normal_velocity < 0.) for (int d=0;d<3;++d) node.v[d]-=normal_velocity*contact_normal[d];
}

void MeshModuleFabric::initial_integrate(int) {
  if(!initialized_) return;
  const double dt=update->dt, maximum=.25*neighbor->skin;
  for(std::vector<Node>::iterator it=nodes_.begin();it!=nodes_.end();++it) {
    if(it->pinned) continue;
    for(int d=0;d<3;++d) {
      it->v[d]+=.5*dt*it->f[d]/node_mass_;
      const double delta=dt*it->v[d];
      if(maximum>0.&&std::fabs(delta)>maximum)
        error->all(FLERR,"Fabric node moved too far for the neighbour skin; reduce timestep or spring stiffness");
      it->x[d]+=delta;
    }
    apply_supports(*it);
  }
  write_nodes_to_mesh();
  fabric_mesh_->refresh_fabric_geometry();
  set_material_velocity();
  *fix_mesh->get_next_reneighbor_ptr()=update->ntimestep+1;
}

void MeshModuleFabric::write_nodes_to_mesh() {
  for(int i=0;i<fabric_mesh_->sizeLocal();++i)
    for(int j=0;j<3;++j)
      fabric_mesh_->set_fabric_node(i,j,nodes_[tri_nodes_[i][j]].x.data());
}

void MeshModuleFabric::calculate_internal_forces() {
  for(std::vector<Edge>::const_iterator edge=edges_.begin();edge!=edges_.end();++edge) {
    Node &a=nodes_[edge->a], &b=nodes_[edge->b];
    std::array<double,3> unit={{b.x[0]-a.x[0],b.x[1]-a.x[1],b.x[2]-a.x[2]}};
    const double length=magnitude(unit);
    if(length<1.e-14) error->all(FLERR,"Fabric spring collapsed to zero length");
    for(int d=0;d<3;++d) unit[d]/=length;
    const double relative=(b.v[0]-a.v[0])*unit[0]+(b.v[1]-a.v[1])*unit[1]+(b.v[2]-a.v[2])*unit[2];
    const double scale=edge->pre_tension+edge->spring*(length-edge->rest)+edge->damping*relative;
    for(int d=0;d<3;++d) {
      const double f=scale*unit[d];
      a.f[d]+=f;
      b.f[d]-=f;
    }
  }
  calculate_bending_forces();
}

double MeshModuleFabric::bending_energy(const Bend &bend) const {
  const double angle=dihedral_angle(nodes_[bend.a].x,nodes_[bend.b].x,nodes_[bend.c].x,nodes_[bend.d].x);
  const double delta=angle-bend.rest_angle;
  return 0.5*bending_*delta*delta;
}

void MeshModuleFabric::calculate_bending_forces() {
  if (bending_ == 0.) return;
  for (std::vector<Bend>::const_iterator bend=bends_.begin(); bend!=bends_.end(); ++bend) {
    const int index[4]={bend->a,bend->b,bend->c,bend->d};
    double angle, gradient[12];
    if (!dihedral_angle_gradient(nodes_[index[0]].x,nodes_[index[1]].x,nodes_[index[2]].x,nodes_[index[3]].x,angle,gradient)) continue;
    const double scale=-bending_*(angle-bend->rest_angle);
    for (int vertex=0; vertex<4; ++vertex)
      for (int component=0; component<3; ++component)
        nodes_[index[vertex]].f[component]+=scale*gradient[3*vertex+component];
  }
}

void MeshModuleFabric::final_integrate() {
  if(!initialized_) return;
  calculate_internal_forces();
  const double scale=.5*update->dt/node_mass_;
  for(std::vector<Node>::iterator it=nodes_.begin();it!=nodes_.end();++it) {
    if(it->pinned) {
      it->v={{0.,0.,0.}};
      continue;
    }
    for(int d=0;d<3;++d) it->v[d]+=scale*it->f[d];
  }
}

void MeshModuleFabric::add_particle_contribution(int ip,double *particle_force,double *delta,
                                                  int iTri,double *) {
  if(!initialized_||iTri<0||iTri>=static_cast<int>(tri_nodes_.size())) return;
  double x0[3],point[3],from_x0[3],edge_store[3][3],edge_length[3],bary[3];
  double *edge_vector[3]={edge_store[0],edge_store[1],edge_store[2]};
  fabric_mesh_->fabric_node(iTri,0,x0);
  for(int d=0;d<3;++d) {
    point[d]=atom->x[ip][d]+delta[d];
    from_x0[d]=point[d]-x0[d];
  }
  fabric_mesh_->edgeVec(iTri,0,edge_store[0]);
  fabric_mesh_->edgeVec(iTri,1,edge_store[1]);
  fabric_mesh_->edgeVec(iTri,2,edge_store[2]);
  fabric_mesh_->edgeLen(iTri,edge_length);
  MathExtraLiggghts::calcBaryTriCoords(from_x0,edge_vector,edge_length,bary);
  for(int j=0;j<3;++j) {
    Node &node=nodes_[tri_nodes_[iTri][j]];
    for(int d=0;d<3;++d) node.f[d]-=bary[j]*particle_force[d];
  }
}
