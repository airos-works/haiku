"""rk3588_vpu completion interrupts (#182) against a fake block and interrupt controller."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class VpuCompletionTest(unittest.TestCase):
    def test_handler_wait_trust_and_storm(self):
        directory = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory(prefix='vpu-completion-') as temporary:
            binary = Path(temporary) / 'completion-test'
            built = subprocess.run([
                'g++', '-pthread', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                str(directory / 'test_vpu_completion.cpp'), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                timeout=120, env=dict(os.environ,
                    ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                    UBSAN_OPTIONS='halt_on_error=1'))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('RK3588_VPU_COMPLETION_TEST_PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
