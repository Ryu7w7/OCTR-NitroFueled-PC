"""run.py's watch over Vulkan's start-up: a game stuck on a black window (no GPU opened) starts again
without the overlay layers; a normal start is left alone."""
from paths import ROOT
import subprocess
import sys
import time
import unittest

sys.path.insert(0, str(ROOT))
import run  # noqa: E402


def sleeper(seconds):
    return subprocess.Popen([sys.executable, '-c', f'import time; time.sleep({seconds})'])


class VulkanStartTests(unittest.TestCase):
    def setUp(self):
        self.said = []
        self.say = run.say
        run.say = self.said.append
        self.seconds = run.VULKAN_START_SECONDS
        run.VULKAN_START_SECONDS = 1

    def tearDown(self):
        run.say = self.say
        run.VULKAN_START_SECONDS = self.seconds
        run.VULKAN_START.update(window=0.0, ready=False, layers='')

    def test_hung_start_is_retried_without_layers(self):
        run.emit('Window: 1467x825 logical\n')
        run.emit('Vulkan: starting (loader 1.4.0); layers installed: VK_LAYER_OBS_hook\n')
        run.VULKAN_START['window'] -= 5
        process = sleeper(30)
        started = time.time()
        self.assertTrue(run.watch_vulkan_start(process, {}))
        self.assertLess(time.time() - started, 10)
        self.assertIsNotNone(process.poll())  # closed
        self.assertIn('VK_LAYER_OBS_hook', self.said[0])

    def test_normal_start_is_left_alone(self):
        run.emit('Window: 1467x825 logical\n')
        run.emit('GPU: NVIDIA GeForce GTX 1650, 3935 MiB VRAM; NVIDIA 610.88, Vulkan 1.4.341\n')
        run.VULKAN_START['window'] -= 5
        self.assertFalse(run.watch_vulkan_start(sleeper(2), {}))
        self.assertEqual(self.said, [])

    def test_second_hang_is_only_reported(self):
        run.emit('Window: 1467x825 logical\n')
        run.VULKAN_START['window'] -= 5
        self.assertFalse(run.watch_vulkan_start(sleeper(3), {'VK_LOADER_LAYERS_DISABLE': '~implicit~'}))
        self.assertEqual(len(self.said), 1)
        self.assertIn('driver', self.said[0])


if __name__ == '__main__':
    unittest.main()
