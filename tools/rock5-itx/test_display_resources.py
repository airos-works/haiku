"""Exercise the production RK3588 display admission, ioctl and read-only observation."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class DisplayResourcesTest(unittest.TestCase):
    def test_display_graph_observation_and_ioctl_boundaries(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/add-ons/kernel/drivers/graphics/rk3588_display'
        code = (source / 'driver.cpp').read_text().replace(
            '#include "DisplayHardware.h"', (source / 'DisplayHardware.h').read_text())
        with tempfile.TemporaryDirectory(prefix='display-resources-') as temporary:
            root = Path(temporary)
            # FDT providers, kernel mapping/area services and user copy are fixtures.
            # Traversal, register offsets, gating, cleanup paths and ioctl are unchanged.
            (root / 'driver.inc').write_text(
                code[code.index('static device_manager_info*'):
                     code.index('static float\nSupportsDevice')]
                + code[code.index('static status_t\nControl('):
                       code.index('module_dependency module_dependencies[]')])
            binary = root / 'display-test'
            built = subprocess.run([
                'g++', '-pthread', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                '-I', str(source), '-I', str(root),
                '-I', str(directory.parents[1] / 'headers' / 'private' / 'graphics'
                    / 'common'),
                str(directory / 'test_display_resources.cpp'), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                timeout=60, env=dict(os.environ,
                    ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                    UBSAN_OPTIONS='halt_on_error=1'))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('RK3588_DISPLAY_RESOURCES_TEST_PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
