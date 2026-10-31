#!/usr/bin/env python3
"""A small Tk UI for tuning a ``mesh/surface/fabric`` conveyor belt.

The program uses Packfall's shared library through :class:`packfall.Packfall`.
It stages the selected input with the values in the UI, replaces every ``run``
command with ``run 0`` while loading, and advances the simulation in explicit
chunks from the Run button.  This is deliberate: fabric properties are parsed
when the fix is created, so applying a value starts a fresh Packfall session.

Run from the repository root::

    make -C src -f Makefile.linux shlib
    python python/examples/fabric_tuner.py

An optional input path can be supplied.  The default is the head-pulley fabric
example.  Tkinter is part of standard CPython on the usual Linux and Windows
installations; no Qt dependency is required.
"""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
from dataclasses import dataclass
from typing import Mapping

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
except ImportError:  # Keeps the input-rewriting helpers usable in headless tests.
    tk = None
    filedialog = messagebox = ttk = None


THIS_DIR = Path(__file__).resolve().parent
PYTHON_DIR = THIS_DIR.parent
if str(PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(PYTHON_DIR))

from packfall import Packfall


DEFAULT_INPUT = THIS_DIR.parents[1] / "examples" / "fabric_head_pulley" / "in.fabric_head_pulley"


@dataclass(frozen=True)
class FabricField:
    key: str
    label: str
    unit: str
    default: float
    minimum: float = 0.0


FIELDS = (
    FabricField("speed", "Belt speed", "m/s", 0.10),
    FabricField("node_mass", "Node mass", "kg", 1.0e-5, 1.0e-15),
    FabricField("spring_longitudinal", "Longitudinal stiffness", "N/m", 8.0, 1.0e-15),
    FabricField("spring_transverse", "Transverse stiffness", "N/m", 2.0, 1.0e-15),
    FabricField("spring_shear", "Shear stiffness", "N/m", 0.08, 1.0e-15),
    FabricField("damping_longitudinal", "Longitudinal damping", "N s/m", 0.018),
    FabricField("damping_transverse", "Transverse damping", "N s/m", 0.009),
    FabricField("damping_shear", "Shear damping", "N s/m", 0.002),
    FabricField("pre_tension_longitudinal", "Longitudinal pre-tension", "N", 1.0e-4),
    FabricField("bending", "Bending stiffness", "N m/rad²", 1.0e-5),
)


class InputError(ValueError):
    """The selected input cannot be safely tuned by this small UI."""


def fabric_command_span(text: str) -> tuple[int, int]:
    """Return the character range for the logical fabric ``fix`` command."""
    lines = text.splitlines(keepends=True)
    offset = 0
    for start, line in enumerate(lines):
        if "mesh/surface/fabric" not in line:
            offset += len(line)
            continue
        end = start + 1
        while lines[end - 1].rstrip().endswith("&"):
            if end == len(lines):
                raise InputError("fabric fix ends with an unfinished continuation")
            end += 1
        return offset, offset + sum(len(item) for item in lines[start:end])
    raise InputError("no mesh/surface/fabric fix was found")


def _replace_number(command: str, keyword: str, value: float) -> tuple[str, bool]:
    pattern = re.compile(r"(\b" + re.escape(keyword) + r"\s+)([^\s#&]+)")
    return pattern.subn(r"\g<1>" + format(value, ".12g"), command, count=1)


def rewrite_fabric_input(text: str, values: Mapping[str, float], *, idle: bool = True) -> str:
    """Return ``text`` with fabric keywords updated from ``values``.

    Only known fabric keywords may be supplied and each must already occur in
    the selected input.  That guard avoids silently generating a different
    belt model from an input whose topology or option set is unfamiliar.
    """
    allowed = {field.key for field in FIELDS}
    unknown = sorted(set(values) - allowed)
    if unknown:
        raise InputError("unsupported fabric setting: " + ", ".join(unknown))

    start, end = fabric_command_span(text)
    command = text[start:end]
    for key, value in values.items():
        command, count = _replace_number(command, key, value)
        if not count:
            raise InputError(f"the fabric fix does not contain '{key}'")

    result = text[:start] + command + text[end:]
    if idle:
        # Loading an input through the library must not unexpectedly run an
        # old production-sized job.  Keep comments and indentation intact.
        result = re.sub(r"(?m)^(\s*)run\s+[^#\r\n]*(?=\s*(?:#|$))", r"\1run 0", result)
    return result


def read_fabric_values(path: Path) -> dict[str, float]:
    """Read the UI fields present in a fabric fix, using documented defaults."""
    text = path.read_text(encoding="utf-8")
    start, end = fabric_command_span(text)
    command = text[start:end]
    values = {field.key: field.default for field in FIELDS}
    for field in FIELDS:
        match = re.search(r"\b" + re.escape(field.key) + r"\s+([^\s#&]+)", command)
        if match:
            try:
                values[field.key] = float(match.group(1))
            except ValueError as exc:
                raise InputError(f"{field.key} is not a numeric value") from exc
    return values


def fabric_fix_id(text: str) -> str:
    """Return the ID of the ``mesh/surface/fabric`` fix in ``text``."""
    start, end = fabric_command_span(text)
    match = re.search(r"^\s*fix\s+(\S+)\s+\S+\s+mesh/surface/fabric\b", text[start:end])
    if not match:
        raise InputError("could not determine the fabric fix ID")
    return match.group(1)


def strip_dump_commands(text: str) -> str:
    """Remove logical ``dump`` commands from a staged input.

    The interactive session installs its own isolated fabric STL dump, so a
    preview never writes over the input's production output directory.
    """
    result: list[str] = []
    lines = text.splitlines(keepends=True)
    index = 0
    while index < len(lines):
        line = lines[index]
        if not re.match(r"^\s*dump\s+", line):
            result.append(line)
            index += 1
            continue
        index += 1
        while line.rstrip().endswith("&"):
            if index == len(lines):
                raise InputError("dump command ends with an unfinished continuation")
            line = lines[index]
            index += 1
    return "".join(result)


def read_ascii_stl_triangles(path: Path) -> list[tuple[tuple[float, float, float], ...]]:
    """Read the triangle vertices from Packfall's ASCII mesh/STL output."""
    vertices: list[tuple[float, float, float]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[0] == "vertex":
            try:
                vertices.append((float(fields[1]), float(fields[2]), float(fields[3])))
            except ValueError as exc:
                raise InputError(f"invalid STL vertex in {path}") from exc
    if not vertices or len(vertices) % 3:
        raise InputError(f"{path} does not contain complete ASCII STL triangles")
    return [tuple(vertices[index : index + 3]) for index in range(0, len(vertices), 3)]


class FabricView(tk.Canvas if tk else object):
    """Small, dependency-free orbit view for a fabric STL mesh."""
    def __init__(self, master) -> None:
        super().__init__(master, background="#f5f1e8", highlightthickness=1, highlightbackground="#9a9388", height=370)
        self.triangles: list[tuple[tuple[float, float, float], ...]] = []
        self.atoms: list[tuple[float, float, float, float]] = []
        self.step: int | None = None
        self.yaw = -0.65
        self.pitch = 0.55
        self.zoom = 1.0
        self._drag_at: tuple[int, int] | None = None
        self.bind("<Configure>", lambda _event: self.render())
        self.bind("<ButtonPress-1>", self._begin_drag)
        self.bind("<B1-Motion>", self._drag)
        self.bind("<MouseWheel>", self._wheel)
        self.bind("<Button-4>", lambda _event: self._change_zoom(1.1))
        self.bind("<Button-5>", lambda _event: self._change_zoom(1 / 1.1))

    def set_mesh(
        self,
        triangles: list[tuple[tuple[float, float, float], ...]],
        step: int,
        atoms: list[tuple[float, float, float, float]],
    ) -> None:
        self.triangles = triangles
        self.step = step
        self.atoms = atoms
        self.render()

    def _begin_drag(self, event) -> None:
        self._drag_at = (event.x, event.y)

    def _drag(self, event) -> None:
        if self._drag_at is None:
            return
        x, y = self._drag_at
        self.yaw += (event.x - x) * 0.012
        self.pitch = max(-1.45, min(1.45, self.pitch + (event.y - y) * 0.012))
        self._drag_at = (event.x, event.y)
        self.render()

    def _wheel(self, event) -> None:
        self._change_zoom(1.1 if event.delta > 0 else 1 / 1.1)

    def _change_zoom(self, factor: float) -> None:
        self.zoom = max(0.2, min(8.0, self.zoom * factor))
        self.render()

    def _camera(self, point: tuple[float, float, float]) -> tuple[float, float, float]:
        x, y, z = point
        cosine, sine = math.cos(self.yaw), math.sin(self.yaw)
        x, y = cosine * x - sine * y, sine * x + cosine * y
        cosine, sine = math.cos(self.pitch), math.sin(self.pitch)
        return x, cosine * y - sine * z, sine * y + cosine * z

    def render(self) -> None:
        self.delete("scene")
        width, height = max(self.winfo_width(), 2), max(self.winfo_height(), 2)
        if not self.triangles:
            self.create_text(width / 2, height / 2, text="Apply parameters to load the fabric preview", fill="#625d55", tags="scene")
            return
        camera_triangles = [[self._camera(point) for point in triangle] for triangle in self.triangles]
        camera_atoms = [(self._camera(atom[:3]), atom[3]) for atom in self.atoms]
        flat = [point for triangle in camera_triangles for point in triangle] + [point for point, _radius in camera_atoms]
        xcentre = (min(point[0] for point in flat) + max(point[0] for point in flat)) / 2
        ycentre = (min(point[1] for point in flat) + max(point[1] for point in flat)) / 2
        xspan = max(max(point[0] for point in flat) - min(point[0] for point in flat), 1.0e-12)
        yspan = max(max(point[1] for point in flat) - min(point[1] for point in flat), 1.0e-12)
        scale = min((width - 36) / xspan, (height - 36) / yspan) * self.zoom
        for triangle in sorted(camera_triangles, key=lambda item: sum(point[2] for point in item) / 3):
            polygon = [coordinate for point in triangle for coordinate in (width / 2 + (point[0] - xcentre) * scale, height / 2 - (point[1] - ycentre) * scale)]
            self.create_polygon(polygon, fill="#d37d4e", outline="#6a3928", width=1, tags="scene")
        for point, radius in sorted(camera_atoms, key=lambda item: item[0][2]):
            x = width / 2 + (point[0] - xcentre) * scale
            y = height / 2 - (point[1] - ycentre) * scale
            pixel_radius = max(3, min(abs(radius * scale), 30))
            self.create_oval(x - pixel_radius, y - pixel_radius, x + pixel_radius, y + pixel_radius, fill="#3e78a8", outline="#173c5f", tags="scene")
        self.create_text(10, 10, anchor="nw", text=f"Fabric + DEM particles · step {self.step}   Drag: orbit   Wheel: zoom", fill="#302d29", tags="scene")


class FabricTuner(tk.Tk if tk else object):
    def __init__(self, input_path: Path) -> None:
        if tk is None:
            raise RuntimeError("Tkinter is not installed")
        super().__init__()
        self.title("Packfall Fabric Tuner")
        self.minsize(700, 760)
        self.sim: Packfall | None = None
        self.preview_dir: Path | None = None
        self.live = False
        self.frame_steps: int | None = None
        self.input_path = input_path.resolve()
        self.variables = {field.key: tk.StringVar(value=format(field.default, ".8g")) for field in FIELDS}
        self.path_var = tk.StringVar(value=str(self.input_path))
        self.status_var = tk.StringVar(value="Choose values, then Apply / reload.")
        self.steps_var = tk.StringVar(value="100")
        self.critical_var = tk.StringVar()
        self._build()
        self._load_values_from_input()

    def _build(self) -> None:
        root = ttk.Frame(self, padding=12)
        root.grid(sticky="nsew")
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)
        root.columnconfigure(1, weight=1)

        ttk.Label(root, text="Fabric input").grid(row=0, column=0, sticky="w")
        ttk.Entry(root, textvariable=self.path_var, state="readonly").grid(row=0, column=1, sticky="ew", padx=8)
        ttk.Button(root, text="Choose…", command=self.choose_input).grid(row=0, column=2)

        parameter_box = ttk.LabelFrame(root, text="Fabric / conveyor-belt parameters", padding=10)
        parameter_box.grid(row=1, column=0, columnspan=3, sticky="nsew", pady=(12, 0))
        parameter_box.columnconfigure(1, weight=1)
        for row, field in enumerate(FIELDS):
            ttk.Label(parameter_box, text=field.label).grid(row=row, column=0, sticky="w", pady=2)
            entry = ttk.Entry(parameter_box, textvariable=self.variables[field.key], width=18)
            entry.grid(row=row, column=1, sticky="ew", padx=8, pady=2)
            ttk.Label(parameter_box, text=field.unit).grid(row=row, column=2, sticky="w")
            self.variables[field.key].trace_add("write", self._update_critical_damping)

        ttk.Label(root, textvariable=self.critical_var, justify="left").grid(
            row=2, column=0, columnspan=3, sticky="w", pady=(8, 0)
        )
        preview_box = ttk.LabelFrame(root, text="Live fabric preview", padding=6)
        preview_box.grid(row=3, column=0, columnspan=3, sticky="nsew", pady=(10, 0))
        preview_box.columnconfigure(0, weight=1)
        self.preview = FabricView(preview_box)
        self.preview.grid(row=0, column=0, sticky="nsew")
        actions = ttk.Frame(root)
        actions.grid(row=4, column=0, columnspan=3, sticky="ew", pady=(12, 0))
        ttk.Button(actions, text="Apply / reload", command=self.apply).grid(row=0, column=0)
        ttk.Label(actions, text="Steps / frame").grid(row=0, column=1, padx=(18, 5))
        ttk.Entry(actions, textvariable=self.steps_var, width=8).grid(row=0, column=2)
        ttk.Button(actions, text="Run one frame", command=self.run_steps).grid(row=0, column=3, padx=6)
        ttk.Button(actions, text="Start live", command=self.start_live).grid(row=0, column=4, padx=(12, 0))
        ttk.Button(actions, text="Stop", command=self.stop_live).grid(row=0, column=5, padx=6)
        ttk.Button(actions, text="Save tuned input…", command=self.save_input).grid(row=0, column=6, padx=(12, 0))
        ttk.Button(actions, text="Close", command=self.destroy).grid(row=0, column=7, padx=(6, 0))

        ttk.Label(root, textvariable=self.status_var, wraplength=610).grid(
            row=5, column=0, columnspan=3, sticky="w", pady=(12, 0)
        )

    def _values(self) -> dict[str, float]:
        values: dict[str, float] = {}
        for field in FIELDS:
            try:
                value = float(self.variables[field.key].get())
            except ValueError as exc:
                raise InputError(f"{field.label} must be a number") from exc
            if not math.isfinite(value) or value < field.minimum:
                qualifier = "> 0" if field.minimum > 0 else "≥ 0"
                raise InputError(f"{field.label} must be finite and {qualifier}")
            values[field.key] = value
        return values

    def _update_critical_damping(self, *_unused) -> None:
        try:
            values = self._values()
        except InputError:
            self.critical_var.set("Enter valid values to see critical-damping estimates.")
            return
        mass = values["node_mass"]
        self.critical_var.set(
            "Single-edge critical damping estimate (N s/m):  "
            f"longitudinal {2 * math.sqrt(values['spring_longitudinal'] * mass):.4g};  "
            f"transverse {2 * math.sqrt(values['spring_transverse'] * mass):.4g};  "
            f"shear {2 * math.sqrt(values['spring_shear'] * mass):.4g}."
        )

    def _load_values_from_input(self) -> None:
        try:
            for key, value in read_fabric_values(self.input_path).items():
                self.variables[key].set(format(value, ".8g"))
            self.status_var.set("Values read from input. Apply / reload creates an idle library session.")
        except (OSError, InputError) as exc:
            self.status_var.set(str(exc))
            messagebox.showerror("Cannot read fabric input", str(exc), parent=self)
        self._update_critical_damping()

    def choose_input(self) -> None:
        chosen = filedialog.askopenfilename(
            parent=self,
            title="Choose a Packfall fabric input",
            initialdir=str(self.input_path.parent),
            filetypes=(("Packfall inputs", "in.* *.in *.txt"), ("All files", "*")),
        )
        if chosen:
            self.input_path = Path(chosen).resolve()
            self.path_var.set(str(self.input_path))
            self._load_values_from_input()

    def _staged_input(self, *, idle: bool) -> tuple[Path, str]:
        source = self.input_path.read_text(encoding="utf-8")
        fix_id = fabric_fix_id(source)
        tuned = rewrite_fabric_input(strip_dump_commands(source), self._values(), idle=idle)
        handle = tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", prefix="packfall-fabric-tuner-", suffix=".in", delete=False
        )
        with handle:
            handle.write(tuned)
        return Path(handle.name), fix_id

    def _steps_per_frame(self) -> int:
        try:
            steps = int(self.steps_var.get())
        except ValueError as exc:
            raise InputError("Steps / frame must be a positive integer") from exc
        if steps < 1:
            raise InputError("Steps / frame must be a positive integer")
        return steps

    def _remove_preview_output(self) -> None:
        if self.preview_dir is not None:
            shutil.rmtree(self.preview_dir, ignore_errors=True)
            self.preview_dir = None

    def _refresh_preview(self) -> None:
        if self.preview_dir is None:
            return
        snapshots = list(self.preview_dir.glob("fabric_*.stl"))
        if not snapshots:
            return
        newest = max(snapshots, key=lambda path: path.stat().st_mtime_ns)
        step_match = re.search(r"fabric_(\d+)\.stl$", newest.name)
        if step_match is None:
            return
        self.preview.set_mesh(read_ascii_stl_triangles(newest), int(step_match.group(1)), self._atom_view_data())

    def _atom_view_data(self) -> list[tuple[float, float, float, float]]:
        if self.sim is None:
            return []
        atoms = self.sim.get_natoms()
        if not atoms:
            return []
        coordinates = self.sim.gather_atoms("x", 1, 3)
        radii = self.sim.gather_atoms("radius", 1, 1)
        return [
            (coordinates[3 * index], coordinates[3 * index + 1], coordinates[3 * index + 2], radii[index])
            for index in range(atoms)
        ]

    def apply(self) -> None:
        staged: Path | None = None
        try:
            self.stop_live()
            staged, fix_id = self._staged_input(idle=True)
            frame_steps = self._steps_per_frame()
            if self.sim is not None:
                self.sim.close()
            self._remove_preview_output()
            # Packfall resolves relative meshes/includes/output paths from the
            # selected input's directory, just as a command-line run does.
            os.chdir(self.input_path.parent)
            # ``-screen none`` currently crashes this shared-library build as
            # soon as a staged fabric input is read.  Disabling only the log
            # file is safe; keep normal screen handling for the library.
            self.sim = Packfall(cmdargs=["-log", "none"])
            self.sim.file(str(staged))
            self.preview_dir = Path(tempfile.mkdtemp(prefix="packfall-fabric-preview-"))
            output = (self.preview_dir / "fabric_*.stl").as_posix()
            self.sim.command(f"dump fabric_tuner_preview all mesh/stl {frame_steps} {output} {fix_id}")
            self.sim.command("run 0")
            self.frame_steps = frame_steps
            self._refresh_preview()
            step = self.sim.extract_global("ntimestep", 0)
            self.status_var.set(f"Loaded step {step}; preview is isolated in a temporary directory. Start live runs {frame_steps}-step frames.")
        except Exception as exc:
            self.status_var.set(f"Load failed: {exc}")
            messagebox.showerror("Packfall load failed", str(exc), parent=self)
            if self.sim is not None:
                self.sim.close()
                self.sim = None
        finally:
            if staged is not None:
                staged.unlink(missing_ok=True)

    def _run_frame(self) -> int:
        if self.sim is None or self.frame_steps is None:
            raise InputError("Apply / reload before running")
        os.chdir(self.input_path.parent)
        self.sim.command(f"run {self.frame_steps}")
        self._refresh_preview()
        return self.sim.extract_global("ntimestep", 0)

    def run_steps(self) -> None:
        if self.sim is None:
            messagebox.showinfo("Apply parameters first", "Apply / reload before running.", parent=self)
            return
        try:
            if self.frame_steps != self._steps_per_frame():
                raise InputError("Apply / reload after changing Steps / frame")
            self.status_var.set(f"Running {self.frame_steps} steps…")
            self.update_idletasks()
            step = self._run_frame()
            self.status_var.set(f"Preview updated at step {step}.")
        except Exception as exc:
            self.status_var.set(f"Run failed: {exc}")
            messagebox.showerror("Packfall run failed", str(exc), parent=self)

    def start_live(self) -> None:
        if self.sim is None:
            messagebox.showinfo("Apply parameters first", "Apply / reload before starting a live run.", parent=self)
            return
        try:
            if self.frame_steps != self._steps_per_frame():
                raise InputError("Apply / reload after changing Steps / frame")
        except InputError as exc:
            messagebox.showerror("Cannot start live run", str(exc), parent=self)
            return
        if not self.live:
            self.live = True
            self.status_var.set("Live run started. Stop is checked between preview frames.")
            self.after_idle(self._live_tick)

    def stop_live(self) -> None:
        if self.live:
            self.live = False
            self.status_var.set("Live run will stop after the current preview frame.")

    def _live_tick(self) -> None:
        if not self.live:
            return
        try:
            step = self._run_frame()
            self.status_var.set(f"Live preview updated at step {step}. Stop is checked between frames.")
        except Exception as exc:
            self.live = False
            self.status_var.set(f"Live run failed: {exc}")
            messagebox.showerror("Packfall live run failed", str(exc), parent=self)
            return
        self.after(1, self._live_tick)

    def save_input(self) -> None:
        try:
            target = filedialog.asksaveasfilename(
                parent=self,
                title="Save tuned Packfall input",
                initialdir=str(self.input_path.parent),
                initialfile=f"{self.input_path.stem}_tuned.in",
                defaultextension=".in",
                filetypes=(("Packfall inputs", "*.in"), ("All files", "*")),
            )
            if not target:
                return
            Path(target).write_text(
                rewrite_fabric_input(self.input_path.read_text(encoding="utf-8"), self._values(), idle=False),
                encoding="utf-8",
            )
            self.status_var.set(f"Saved tuned input to {target}")
        except Exception as exc:
            self.status_var.set(f"Save failed: {exc}")
            messagebox.showerror("Cannot save tuned input", str(exc), parent=self)

    def destroy(self) -> None:
        self.live = False
        if self.sim is not None:
            self.sim.close()
            self.sim = None
        self._remove_preview_output()
        super().destroy()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Tune a Packfall mesh/surface/fabric input through the shared library.")
    parser.add_argument("input", nargs="?", type=Path, default=DEFAULT_INPUT, help="fabric input (default: head-pulley example)")
    args = parser.parse_args(argv)
    if tk is None:
        print("Tkinter is required for fabric_tuner.py. Install the python3-tk package.", file=sys.stderr)
        return 1
    app = FabricTuner(args.input)
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
