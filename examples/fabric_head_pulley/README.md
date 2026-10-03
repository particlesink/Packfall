# Fabric strip over two troughing-idler stations

Run from the repository root:

```sh
cd examples/fabric_head_pulley
../../build/linux/packfall -in in.fabric_head_pulley
```

The example is a 0.16 m by 0.08 m strip with 24 longitudinal by 8 transverse
cells. Only the start and end edges are pinned. Two three-roll trough stations
at x = -0.04 m and x = 0.04 m support the strip; each has a horizontal centre
roller and side rollers rising outwards at 30 degrees. The six fixed support
meshes are listed through repeated `support_mesh` keywords.

## Fabric parameters

| Parameter | Meaning | Units |
| --- | --- | --- |
| `node_mass` | Lumped mass assigned to every structural node. | kg |
| `spring_longitudinal` | Axial stiffness of edges aligned to `transport_direction`. | N/m |
| `spring_transverse` | Stiffness of cross-belt edges. | N/m |
| `spring_shear` | Stiffness of diagonal edges. | N/m |
| `pre_tension_longitudinal` | Constant tensile force added to longitudinal edges. | N |
| `bending` | Hinge stiffness about each initial triangle-edge dihedral angle. | N m/rad2 |
| `damping_longitudinal`, `damping_transverse`, `damping_shear` | Dashpot coefficient for the matching edge class. | N s/m |

`spring` and `damping` remain compatibility fallbacks. A supplied directional
value overrides its matching fallback; unspecified directional values inherit
from `spring` or `damping`. Directional stiffness, damping, longitudinal
tension, and material speed require `transport_direction`.

A useful first damping estimate for one edge is critical damping,
`c_critical = 2 sqrt(k node_mass)`. For the current unloaded strip this is
about 0.018 N s/m longitudinally, 0.009 N s/m transversely, and 0.0018 N s/m
in shear. These are starting estimates only: each node is connected to several
edges, so settle by observing the complete belt. Damping controls how the belt
settles, not its final static shape. Excessive shear damping is especially easy
to introduce when a single uniform damping value is used with a much softer
shear spring.

To reduce span sag, establish longitudinal pre-tension first, then raise
longitudinal stiffness if the belt is stretching too much. Use transverse and
shear stiffness to control trough retention and skew between rollers. Increase
`bending` only when curvature is too sharp or the belt visibly facets; it is not
a substitute for membrane tension. Raising stiffness or reducing `node_mass`
raises natural frequencies, so reduce `timestep` if the response becomes noisy
or a node crosses the neighbour-skin displacement limit.

## Loading caveat

The behaviour shown while tuning this input is the structural belt response
before it carries a representative DEM particle bed. Gravity acts on
`node_mass`, so this gives the empty-belt sag, tension response, and roller
contact geometry. When actual particles are loaded, their contact forces are
transferred to the fabric nodes and add burden-dependent sag, troughing, and
local deflection. Tune the unloaded belt first, then retune pre-tension,
stiffness, and damping with the intended mass flow and particle inventory.

`speed 0.10 transport_direction 1 0 0` sets moving belt material velocity for
DEM contact. It does not advect the structural mesh. Inspect
`post/fabric_*.stl` for the belt and `post/fabric_troughs_*.vtk` for the belt
and all six supports.
