"""Launcher GPU preferences reach the game without changing driver discovery."""
from paths import ROOT
import ast
import os
from pathlib import Path
import subprocess
import unittest
from unittest import mock


def launcher_helpers(packaged=True, extra=()):
    tree = ast.parse((ROOT / "launcher/sacredfire_launcher.py").read_text(encoding="utf-8"))
    nodes = [n for n in tree.body if
             isinstance(n, ast.FunctionDef) and n.name in ("game_environment", "gpu_index", *extra) or
             isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "DEFAULTS" for t in n.targets)]
    scope = {"os": os, "Path": Path, "PORT_DIR": Path("/package"),
             "PACKAGED": packaged, "menu_language": lambda: "en"}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), "launcher_helpers", "exec"), scope)
    return scope


# Two cards with one name (a 16 GB and an 8 GB model), as bb-gpu-capabilities --gpus lists them.
SAME_NAME = ["NVIDIA GeForce RTX 4070", "NVIDIA GeForce RTX 5060 Ti", "NVIDIA RTX A2000",
             "NVIDIA GeForce RTX 5060 Ti"]


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

    def test_a_unique_name_goes_by_name(self):
        scope = launcher_helpers()
        env = scope["game_environment"]({**scope["DEFAULTS"], "gpu": "NVIDIA RTX A2000"}, SAME_NAME)
        self.assertEqual(env["BB_GPU_NAME"], "NVIDIA RTX A2000")
        self.assertNotIn("BB_GPU_ID", env)

    def test_gpus_that_share_a_name_go_by_their_place(self):
        scope = launcher_helpers()
        settings = {**scope["DEFAULTS"], "gpu": "NVIDIA GeForce RTX 5060 Ti"}
        env = scope["game_environment"](settings, SAME_NAME)  # gpu_nth 0: the first of them
        self.assertEqual(env["BB_GPU_ID"], "1")
        self.assertNotIn("BB_GPU_NAME", env)
        env = scope["game_environment"]({**settings, "gpu_nth": 1}, SAME_NAME)
        self.assertEqual(env["BB_GPU_ID"], "3")
        self.assertNotIn("BB_GPU_NAME", env)

    def test_a_card_that_is_gone_falls_back_to_the_first_with_its_name(self):
        scope = launcher_helpers()
        settings = {**scope["DEFAULTS"], "gpu": "NVIDIA GeForce RTX 5060 Ti", "gpu_nth": 2}
        self.assertEqual(scope["game_environment"](settings, SAME_NAME)["BB_GPU_ID"], "1")
        # One of the two removed: the name is unique again.
        env = scope["game_environment"](settings, SAME_NAME[:3])
        self.assertEqual(env["BB_GPU_NAME"], "NVIDIA GeForce RTX 5060 Ti")
        self.assertNotIn("BB_GPU_ID", env)

    def test_without_the_gpu_list_the_card_goes_by_name(self):
        scope = launcher_helpers()
        settings = {**scope["DEFAULTS"], "gpu": "NVIDIA GeForce RTX 5060 Ti", "gpu_nth": 1}
        for gpus in (None, []):
            env = scope["game_environment"](settings, gpus)
            self.assertEqual(env["BB_GPU_NAME"], "NVIDIA GeForce RTX 5060 Ti")
            self.assertNotIn("BB_GPU_ID", env)

    def test_an_inherited_gpu_index_stays_unless_the_name_is_shared(self):
        scope = launcher_helpers()
        with mock.patch.dict(os.environ, {"BB_GPU_ID": "2", "BB_GPU_NAME": "NVIDIA RTX A2000"}):
            env = scope["game_environment"](dict(scope["DEFAULTS"]), SAME_NAME)  # automatic
            self.assertEqual(env["BB_GPU_ID"], "2")
            self.assertNotIn("BB_GPU_NAME", env)
            env = scope["game_environment"]({**scope["DEFAULTS"], "gpu": "NVIDIA RTX A2000"}, SAME_NAME)
            self.assertEqual(env["BB_GPU_ID"], "2")
            self.assertEqual(env["BB_GPU_NAME"], "NVIDIA RTX A2000")
            env = scope["game_environment"]({**scope["DEFAULTS"], "gpu": "NVIDIA GeForce RTX 5060 Ti",
                                             "gpu_nth": 1}, SAME_NAME)
            self.assertEqual(env["BB_GPU_ID"], "3")
            self.assertNotIn("BB_GPU_NAME", env)

    def test_gpus_that_share_a_name_get_numbered_labels(self):
        output = "".join(f"discrete\t1.4\t{name}\n" for name in SAME_NAME)
        run = mock.Mock(return_value=subprocess.CompletedProcess([], 0, stdout=output))
        scope = launcher_helpers(extra=("connected_gpus",))
        scope.update(subprocess=mock.Mock(run=run, SubprocessError=subprocess.SubprocessError),
                     tr=lambda text: text)
        self.assertEqual(scope["connected_gpus"](), [
            ("NVIDIA GeForce RTX 4070", "NVIDIA GeForce RTX 4070 (dedicated)"),
            ("NVIDIA GeForce RTX 5060 Ti", "NVIDIA GeForce RTX 5060 Ti #1 (dedicated)"),
            ("NVIDIA RTX A2000", "NVIDIA RTX A2000 (dedicated)"),
            ("NVIDIA GeForce RTX 5060 Ti", "NVIDIA GeForce RTX 5060 Ti #2 (dedicated)")])
