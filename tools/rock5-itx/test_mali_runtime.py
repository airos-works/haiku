"""Exercise the production queue API/worker lifetime with OS and GPU fixtures.

Only kernel APIs, CPU cache primitives and the hardware cycle are substituted.
The separate queue test interprets the real hardware scheduling engine.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from mali_sync_fixture import prepare_sync_fixture


class MaliRuntimeTest(unittest.TestCase):
    def test_workers_leases_waits_close_and_failure_retention(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/add-ons/kernel/drivers/graphics/mali_csf'
        code = (source / 'CsfRuntime.cpp').read_text()
        code = code[code.index('using namespace MaliCSF;'):]
        code = code.replace('#include "CsfFirmwareHardware.h"', '')
        code = code.replace('#include "CsfDmaMemory.h"', (source / 'CsfDmaMemory.h').read_text())
        begin = code.index('static status_t\nMakeFirmwareRamNoncacheable')
        end = code.index('template<typename Memory>\nstatic area_id\nAllocateFirmwareMemory', begin)
        code = code[:begin] + code[end:]
        cycle = ('CycleFirmware(runtime->hardware, runtime->memory, runtime->firmware, operation,\n'
            '\t\t\t\t&recovery);')
        self.assertEqual(code.count(cycle), 1)
        code = code.replace(cycle, 'DriveRuntime(runtime, runtime->firmware, recovery);')
        with tempfile.TemporaryDirectory(prefix='mali-runtime-') as temporary:
            root = Path(temporary)
            prepare_sync_fixture(root, directory.parents[1])
            (root / 'runtime.inc').write_text(code)
            binary = root / 'runtime-test'
            built = subprocess.run([
                'g++', '-pthread', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-dangling-reference', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                '-I', str(source), '-I', str(root), str(directory / 'test_mali_runtime.cpp'),
                str(source / 'CsfFirmware.cpp'), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                timeout=45, env=dict(os.environ,
                    ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                    UBSAN_OPTIONS='halt_on_error=1'))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('MALI_CSF_RUNTIME_TEST_PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
