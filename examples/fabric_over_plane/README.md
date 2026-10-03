# Fabric over a fixed primitive plane

Run from the repository root with:

```sh
cd examples/fabric_over_plane
../../build/linux/packfall -in in.fabric_over_plane
```

The example creates a 12 by 12 square fabric lattice (288 triangles, 169
shared nodes) with `generate plane 0.10 resolution 12`. Boundary-edge
detection pins all 48 perimeter nodes. Gravity moves the 121 interior nodes;
`support_zplane 0.0` prevents them crossing the same fixed `zplane 0.0`
primitive used by `wall/gran`, producing a draped patch.

View `post/fabric_*.stl` to see the deformation. This is an initial structural
support constraint, not a general fabric-to-wall contact model. The new
optional `resolution N` form of `generate plane` is useful for creating a
regular N by N fabric lattice.
