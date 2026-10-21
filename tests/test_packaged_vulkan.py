"""Driver discovery at the AppImage's GTK/game entry point, without a GPU."""
from paths import ROOT
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("bbport_vulkan", ROOT / "launcher/bbport_vulkan.py")
vulkan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vulkan)


class PackagedVulkanTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.icds = self.root / "icds"
        self.libs = self.root / "host-libs"
        self.egl = self.root / "egl_vendor.d"
        self.icds.mkdir()
        self.libs.mkdir()
        self.egl.mkdir()
        self.env = {"BB_DATA_DIR": str(self.root / "data"),
                    "BB_BUNDLED_VK_DRIVER_FILES": "/bundled/radeon.json:/bundled/intel.json",
                    "LD_LIBRARY_PATH": "/bundled/lib"}

    def library(self, name, bits=64):
        path = self.libs / name
        path.write_bytes(b"\x7fELF" + bytes([2 if bits == 64 else 1, 1]) +
                         b"\0" * 12 + b"\x3e\0")
        return path

    def manifest(self, library="libGLX_nvidia.so.0"):
        path = self.icds / "nvidia_icd.json"
        path.write_text(json.dumps({"file_format_version": "1.0.0", "ICD": {
            "library_path": library, "api_version": "1.3.0"}}))
        return path

    def configure(self):
        return vulkan.configure(self.env, (self.icds,), (self.libs,), (self.egl,))

    def test_amd_intel_keep_bundled_drivers(self):
        self.configure()
        self.assertEqual(self.env["VK_DRIVER_FILES"], self.env["BB_BUNDLED_VK_DRIVER_FILES"])
        self.assertEqual(self.env["LD_LIBRARY_PATH"], "/bundled/lib")
        self.assertFalse((self.root / "data").exists())

    def test_explicit_and_legacy_overrides_are_preserved(self):
        for key in ("VK_DRIVER_FILES", "VK_ICD_FILENAMES"):
            with self.subTest(key=key):
                self.env[key] = "/user/lavapipe.json"
                before = dict(self.env)
                self.configure()
                self.assertEqual(self.env, before)
                del self.env[key]

    def test_host_nvidia_and_compiler_are_exposed_without_host_libc(self):
        driver = self.library("libGLX_nvidia.so.580.1")
        (self.libs / "libGLX_nvidia.so.0").symlink_to(driver.name)
        compiler = self.library("libnvidia-glvkspirv.so.580.1")
        cuda = self.library("libcuda.so.580.1")
        (self.libs / "libcuda.so.1").symlink_to(cuda.name)
        self.library("libc.so.6")
        self.manifest()
        self.assertIn("host NVIDIA", self.configure())
        driver_json = Path(self.env["VK_DRIVER_FILES"].split(":")[0])
        data = json.loads(driver_json.read_text())
        self.assertEqual(data["ICD"]["api_version"], "1.3.0")
        self.assertEqual(Path(data["ICD"]["library_path"]).resolve(), driver)
        cache = Path(self.env["LD_LIBRARY_PATH"].split(":")[0])
        self.assertEqual((cache / compiler.name).resolve(), compiler)
        # DLSS (NGX) has the driver load libcuda.so.1 (#135).
        self.assertEqual((cache / "libcuda.so.1").resolve(), cuda)
        self.assertFalse((cache / "libc.so.6").exists())
        self.assertTrue(self.env["VK_DRIVER_FILES"].endswith(":/bundled/radeon.json:/bundled/intel.json"))
        # Repeated starts reuse valid links and do not accumulate per-launch caches.
        before = dict(self.env)
        self.env = {k: v for k, v in before.items() if k != "VK_DRIVER_FILES"}
        self.env["LD_LIBRARY_PATH"] = "/bundled/lib"
        self.configure()
        self.assertEqual(self.env, before)

    def test_debian_nvidia_siblings_one_level_up_are_linked(self):
        # Debian 13: the ICD in nvidia/current/, glsi/tls/glcore in the multiarch directory (#205).
        current = self.libs / "nvidia" / "current"
        current.mkdir(parents=True)
        driver = current / "libGLX_nvidia.so.550.163.01"
        driver.write_bytes(self.library("tmp").read_bytes())
        (self.libs / "tmp").unlink()
        (self.libs / "libGLX_nvidia.so.0").symlink_to(driver)
        glsi = self.library("libnvidia-glsi.so.550.163.01")
        own = current / "libnvidia-glcore.so.550.163.01"
        own.write_bytes(glsi.read_bytes())
        self.library("libnvidia-glcore.so.550.163.01")  # the ICD directory's copy wins
        self.manifest()
        self.configure()
        cache = Path(self.env["LD_LIBRARY_PATH"].split(":")[0])
        self.assertEqual((cache / glsi.name).resolve(), glsi)
        self.assertEqual((cache / own.name).resolve(), own)

    def nvidia_host(self):
        driver = self.library("libGLX_nvidia.so.580.1")
        (self.libs / "libGLX_nvidia.so.0").symlink_to(driver.name)
        self.manifest()

    def test_nvidia_gets_the_host_egl_vendors_nvidia_first(self):
        # #107: the package's glvnd saw no host EGL vendor; NVIDIA's ICD gave no vkCreateInstance.
        self.nvidia_host()
        for name in ("50_mesa.json", "10_nvidia.json"):
            (self.egl / name).write_text("{}")
        self.configure()
        self.assertEqual(self.env["__EGL_VENDOR_LIBRARY_FILENAMES"],
                         f"{self.egl / '10_nvidia.json'}:{self.egl / '50_mesa.json'}")

    def test_egl_vendors_left_alone_without_nvidia_or_when_set(self):
        (self.egl / "10_nvidia.json").write_text("{}")
        self.configure() # no NVIDIA ICD: AMD/Intel keep the package's EGL
        self.assertNotIn("__EGL_VENDOR_LIBRARY_FILENAMES", self.env)
        self.nvidia_host()
        for env in ({"__EGL_VENDOR_LIBRARY_FILENAMES": "/user/vendor.json"}, {"BB_NVIDIA_EGL": "0"}):
            with self.subTest(env=env):
                self.env.pop("__EGL_VENDOR_LIBRARY_FILENAMES", None)
                self.env.pop("VK_DRIVER_FILES", None)
                self.env.update(env)
                self.configure()
                self.assertEqual(self.env.get("__EGL_VENDOR_LIBRARY_FILENAMES"),
                                 env.get("__EGL_VENDOR_LIBRARY_FILENAMES"))
                self.env.pop("BB_NVIDIA_EGL", None)

    def test_absolute_and_relative_icd_paths(self):
        driver = self.library("libGLX_nvidia.so.0")
        for name in (str(driver), "../host-libs/" + driver.name):
            with self.subTest(name=name):
                self.manifest(name)
                data, selected = vulkan.host_nvidia((self.icds,), (self.libs,))
                self.assertEqual(selected, driver)

    def test_32_bit_and_malformed_icds_are_skipped(self):
        self.library("libGLX_nvidia.so.0", bits=32)
        self.manifest()
        (self.icds / "broken_nvidia.json").write_text("not JSON")
        self.configure()
        self.assertEqual(self.env["VK_DRIVER_FILES"], self.env["BB_BUNDLED_VK_DRIVER_FILES"])

    def test_additional_user_icds_are_not_lost(self):
        self.env["VK_ADD_DRIVER_FILES"] = "/user/extra.json"
        self.configure()
        self.assertEqual(self.env["VK_DRIVER_FILES"],
                         "/user/extra.json:/bundled/radeon.json:/bundled/intel.json")

    def test_host_driver_can_be_supplied_outside_hidden_nix_store(self):
        self.library("libGLX_nvidia.so.0")
        self.manifest("/nix/store/hidden/lib/libGLX_nvidia.so.0")
        self.env["BB_NVIDIA_LIB_DIR"] = str(self.libs)
        vulkan.configure(self.env, (self.icds,), ())
        self.assertIn("nvidia_icd.json", self.env["VK_DRIVER_FILES"])

    def test_nvidia_report_names_libraries_searched_and_not_loaded(self):
        # Issue #107: the driver loads but gives no vkCreateInstance; the report says what the
        # dynamic linker looked for inside the package and did not load.
        driver = self.library("libGLX_nvidia.so.615.1")
        self.library("libnvidia-glcore.so.615.1")
        self.manifest(str(driver))
        self.configure()
        cache = Path(self.env["LD_LIBRARY_PATH"].split(":")[0])
        trace = "\n".join([
            "   101:\tfind library=libGLdispatch.so.0 [0]; searching",
            "   101:\tcalling init: /bundled/lib/libGLdispatch.so.0",
            "   101:\tfind library=libnvidia-glcore.so.615.1 [0]; searching",
            f"   101:\tcalling init: {cache}/libnvidia-glcore.so.615.1",
            "   101:\tfind library=libnvidia-gpucomp.so.615.1 [0]; searching",
            "   101:\tfind library=libm.so.6 [0]; searching",
        ])
        calls = []

        def run(args, **kwargs):
            calls.append(kwargs["env"].get("LD_DEBUG"))
            return type("Result", (), {"stderr": trace})()

        lines = vulkan.nvidia_report(self.env, "vulkaninfo", run)
        self.assertEqual(calls, ["libs"])
        self.assertTrue(any(line.strip().startswith("libnvidia-glcore.so.615.1 ->") for line in lines))
        self.assertIn("Dynamic linker: 3 NVIDIA/glvnd libraries searched, not loaded: "
                      "libnvidia-gpucomp.so.615.1", lines)

    def test_nvidia_report_is_empty_without_the_host_driver(self):
        self.configure()
        self.assertEqual(vulkan.nvidia_report(self.env, "vulkaninfo", None), [])

    def drm_card(self, name, vendor):
        device = self.root / "drm" / name / "device"
        device.mkdir(parents=True)
        (device / "vendor").write_text(vendor + "\n")

    def test_new_memory_model_offered_with_an_amd_gpu(self):
        self.drm_card("card0", "0x8086")
        self.drm_card("card1", "0x1002")
        self.assertIs(vulkan.amd_gpu(self.root / "drm"), True)

    def test_new_memory_model_not_offered_without_an_amd_gpu(self):
        self.drm_card("card0", "0x10de")
        self.assertIs(vulkan.amd_gpu(self.root / "drm"), False)

    def test_unknown_gpu_leaves_the_choice_to_the_game(self):
        (self.root / "drm").mkdir()
        self.assertIsNone(vulkan.amd_gpu(self.root / "drm"))


class PressureVesselTests(PackagedVulkanTests):
    """Steam's Game Mode (pressure-vessel) sets Vulkan variables of its own (#172)."""
    def test_pressure_vessel_overrides_are_dropped(self):
        env = dict(self.env,
                   VK_DRIVER_FILES="/usr/lib/pressure-vessel/overrides/share/vulkan/icd.d/radeon_icd.x86_64.json",
                   VK_IMPLICIT_LAYER_PATH="/usr/lib/pressure-vessel/overrides/share/vulkan/implicit_layer.d",
                   LD_LIBRARY_PATH="/usr/lib/pressure-vessel/overrides/lib/x86_64-linux-gnu/aliases:/bundled/lib")
        vulkan.configure(env, manifest_dirs=(self.icds,), library_dirs=(self.libs,),
                         egl_dirs=(self.egl,))
        self.assertEqual(env["VK_DRIVER_FILES"], "/bundled/radeon.json:/bundled/intel.json")
        self.assertNotIn("VK_IMPLICIT_LAYER_PATH", env)
        self.assertEqual(env["LD_LIBRARY_PATH"], "/bundled/lib")
