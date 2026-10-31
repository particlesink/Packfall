"""Focused tests for the non-GUI fabric-tuner input preparation."""

from __future__ import annotations

import sys
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / "python" / "examples"
sys.path.insert(0, str(EXAMPLES))

from fabric_tuner import (
    InputError,
    fabric_fix_id,
    read_ascii_stl_triangles,
    read_fabric_values,
    rewrite_fabric_input,
    strip_dump_commands,
)


class FabricTunerInputTest(unittest.TestCase):
    def setUp(self) -> None:
        self.path = ROOT / "examples" / "fabric_head_pulley" / "in.fabric_head_pulley"
        self.source = self.path.read_text(encoding="utf-8")

    def test_reads_example_values(self) -> None:
        values = read_fabric_values(self.path)
        self.assertEqual(values["speed"], 0.10)
        self.assertEqual(values["spring_longitudinal"], 8.0)
        self.assertEqual(values["node_mass"], 1.0e-5)

    def test_rewrites_fabric_without_touching_comments_and_idles_runs(self) -> None:
        tuned = rewrite_fabric_input(
            self.source,
            {"speed": 0.25, "spring_longitudinal": 12.5, "damping_shear": 0.004},
        )
        self.assertIn("speed 0.25 transport_direction", tuned)
        self.assertIn("spring_longitudinal 12.5 spring_transverse 2.0", tuned)
        self.assertIn("damping_shear 0.004", tuned)
        self.assertIn("\nrun 0\n", tuned)
        self.assertIn("speed 0.10 transport_direction", self.source)

    def test_rejects_missing_parameter(self) -> None:
        with self.assertRaisesRegex(InputError, "does not contain 'speed'"):
            rewrite_fabric_input(self.source.replace("speed 0.10 ", ""), {"speed": 0.2})

    def test_prepares_a_clean_preview_input(self) -> None:
        staged = strip_dump_commands(self.source)
        self.assertEqual(fabric_fix_id(staged), "fabric")
        self.assertNotIn("dump fabric_mesh", staged)
        self.assertNotIn("dump fabric_vtk", staged)
        self.assertIn("fix integrate all nve/sphere", staged)

    def test_reads_ascii_stl_triangles(self) -> None:
        stl = """solid sample
facet normal 0 0 1
  outer loop
    vertex 0 0 0
    vertex 1 0 0
    vertex 0 1 0
  endloop
endfacet
endsolid sample
"""
        with tempfile.TemporaryDirectory() as directory:
            mesh = Path(directory) / "sample.stl"
            mesh.write_text(stl, encoding="utf-8")
            triangles = read_ascii_stl_triangles(mesh)

        self.assertEqual(triangles, [((0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0))])


if __name__ == "__main__":
    unittest.main()
