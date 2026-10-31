# Tests

This directory contains the initial Packfall regression/smoke test harness.

## Goals

- keep tests runnable against the current executable-based workflow
- use short deterministic input decks
- cover parser, insertion, contact, dumping, restart, and SPH/SPH-DEM paths in
  the default release gate
- grow toward broader regression and validation coverage over time

## Running

From the repository root:

```sh
make test
```

This builds the Linux executable and runs every test declared in
`tests/manifest.json`. To run tests and the registered example integration
checks together, use `make check`.

Optional arguments:

```sh
python tests/run.py --exe build/msys2/packfall.exe
python tests/run.py --list
python tests/run.py --case superquadric_orientation_modes
python tests/run.py --group smoke
python tests/run.py --group regression
python tests/run.py --group validation-failure
```

The equivalent filtered Make invocation is:

```sh
make test TEST_ARGS='--group smoke'
```

The runner writes captured logs to `tests/artifacts/`.

## Groups

`smoke` is the fast confidence set:

- `sphere_primitive_wall_smoke`
- `sphere_insert_pack_once`
- `superquadric_insert_pack_once`
- `sph_continuity_smoke`
- `sph_stationary_wall_smoke`
- `sph_density_summation_smoke`
- `sph_dem_bridge_smoke`
- `sph_dem_buoyancy_smoke`
- `superquadric_pair_contact_smoke`
- `superquadric_ellipsoid_case`

`sph_dem_bridge_smoke` is the first hybrid SPH-DEM bridge scaffold check:
the fluid group is integrated with `nve/sph`, the solid group with
`nve/sphere`, and `fix couple/sphdem` is expected to transfer momentum from
the carrier phase to the DEM particle.

`regression` covers broader feature and bug-path behavior:

- `sphere_mesh_inclusion_insert`
- `superquadric_orientation_modes`
- `superquadric_template_distributions`
- `superquadric_set_direct`
- `superquadric_mesh_wall`
- `superquadric_dump_schema`
- `superquadric_restart_roundtrip`
- `sphere_restart_roundtrip`
- `sphere_dump_schema`
- `sphere_two_template_distribution`
- `superquadric_two_template_distribution`
- `superquadric_primitive_wall_multihit`
- `run_seconds_vs_steps_equivalence`
- `sph_dem_pressure_gradient_regression`
- `sph_tutorial1_short_regression`

`validation-failure` verifies that bad inputs are rejected with the expected diagnostics:

- `sphere_seed_collision_rejected`
- `sphere_missing_material_rejected`
- `superquadric_seed_collision_rejected`
