"""Launcher GPU preferences reach the game without changing driver discovery."""
from paths import ROOT
import ast
import os
from pathlib import Path
import unittest
from unittest import mock


def launcher_helpers(packaged=True):
    tree = ast.parse((ROOT / "launcher/sacredfire_launcher.py").read_text(encoding="utf-8"))
    nodes = [n for n in tree.body if
             isinstance(n, ast.FunctionDef) and n.name == "game_environment" or
             isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "DEFAULTS" for t in n.targets)]
    scope = {"os": os, "Path": Path, "PORT_DIR": Path("/package"),
             "PACKAGED": packaged, "menu_language": lambda: "en"}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), "launcher_helpers", "exec"), scope)
    return scope


class LauncherGpuTests(unittest.TestCase):
    def test_all_gpu_choices_reach_the_renderer(self):
        scope = launcher_helpers()
        env = scope["game_environment"]({**scope["DEFAULTS"], "gpu": "NVIDIA GeForce GTX 1650"})
        self.assertEqual(env["BB_GPU_NAME"], "NVIDIA GeForce GTX 1650")
        self.assertNotIn("BB_GPU_PREFERENCE", env)
        env = scope["game_environment"](dict(scope["DEFAULTS"]))
        self.assertNotIn("BB_GPU_NAME", env)
        self.assertNotIn("BB_GPU_PREFERENCE", env)
        for choice in ("integrated", "discrete"):  # a choice saved by the earlier selector
            env = scope["game_environment"]({**scope["DEFAULTS"], "gpu_preference": choice})
            self.assertEqual(env["BB_GPU_PREFERENCE"], choice)

    def test_gpu_choice_keeps_packaged_libraries_and_driver_configuration(self):
        scope = launcher_helpers()
        settings = {**scope["DEFAULTS"], "gpu_preference": "discrete"}
        with mock.patch.dict(os.environ, {"LD_LIBRARY_PATH": "/bundled/lib",
                                          "VK_DRIVER_FILES": "/host/nvidia.json"}):
            env = scope["game_environment"](settings)
        self.assertEqual(env["BB_GPU_PREFERENCE"], "discrete")
        self.assertEqual(env["LD_LIBRARY_PATH"], "/bundled/lib")
        self.assertEqual(env["VK_DRIVER_FILES"], "/host/nvidia.json")
