## Packfall

Packfall is a Discrete Particle Simulation code derived from LIGGGHTS-PUBLIC.

## Building

Only two build systems are supported in this tree:

```sh
make linux
make msys2
```

Both entry points compile `src/*.cpp` directly, generate the required style headers via `src/Make.sh`, and write artifacts under `build/linux` or `build/msys2`.

The historical upstream `lib/*` package-library workflow is not used by the
supported Packfall build, with the exception of the retained `lib/poems`
library that backs the in-tree `POEMS` package.

The default build is single-process and uses the bundled MPI shim in `src/STUBS`. Real MPI is optional and must be enabled explicitly:

```sh
make linux
make msys2
make linux USE_MPI=1
make msys2 USE_MPI=1
```

The default release path currently targets sphere, SPH, and SPH-DEM workflows.
Superquadric support remains work in progress and is not part of the default
build-and-smoke gate yet.

## Automated verification
### Sanitizer build

The Linux sanitizer build is isolated under `build/sanitize` and enables
AddressSanitizer and UndefinedBehaviorSanitizer with debug symbols and frame
pointers. It is intended for local investigation and CI smoke checks, not
performance measurements:

```sh
make sanitize
make test-sanitize
make check-sanitize
```

`test-sanitize` runs the test manifest against `build/sanitize/packfall`.
`check-sanitize` also runs the registered examples. Both fail on ASan leaks,
ASan memory errors, and UBSan findings. Pass `TEST_ARGS` or `EXAMPLE_ARGS` to
focus the run, for example `make test-sanitize TEST_ARGS='--group smoke'`.


Run the complete executable-based verification suite from the repository root:

```sh
make check
```

`make check` builds the Linux executable once per suite, then runs every case
registered in `tests/manifest.json` and `examples/manifest.json`. The commands
can also be run independently:

```sh
make test
make examples
```

Use `TEST_ARGS` or `EXAMPLE_ARGS` to pass runner filters, for example
`make test TEST_ARGS='--group smoke'`. Captured output is saved under
`tests/artifacts/` and `examples/artifacts/`; generated simulation files can be
removed with `make clean-tests` or `make clean-examples`.

## Documentation Status

Packfall currently ships a mix of validated workflows, inherited examples, and
legacy manual content carried forward from the LIGGGHTS-PUBLIC codebase.

- The default supported release path is the build-and-smoke workflow described
  above.
- Additional examples and tutorials may still ship even when they are not part
  of the current validation set.
- Built documentation may include inherited LIGGGHTS-era material that remains
  useful for reference, but does not by itself imply that every documented
  feature is currently validated in Packfall.

## Attribution

This repository contains code derived from LIGGGHTS-PUBLIC and LAMMPS. `LIGGGHTS` and `CFDEM` are registered trademarks of DCS Computing GmbH. Packfall is not approved or endorsed by DCS Computing GmbH. See `LICENSE` and source-file headers for license, copyright, and attribution details.
