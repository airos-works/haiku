"""The mali_csf job wait (#182) against a fake GPU and job interrupt, in real time."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class MaliJobWaitTest(unittest.TestCase):
    def test_trust_fallback_and_bounds(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/add-ons/kernel/drivers/graphics/mali_csf'
        with tempfile.TemporaryDirectory(prefix='mali-job-wait-') as temporary:
            binary = Path(temporary) / 'job-wait-test'
            built = subprocess.run([
                'g++', '-pthread', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                '-I', str(source), str(directory / 'test_mali_job_wait.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                timeout=60, env=dict(os.environ,
                    ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                    UBSAN_OPTIONS='halt_on_error=1'))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('MALI_CSF_JOB_WAIT_TEST_PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
