"""Regression coverage for project DEPS and the documented Chromium adaptations."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts/vendor/chromium_checkdeps'))
from checkdeps import DepsChecker
from cpp_checker import CppChecker


class Boundaries(unittest.TestCase):
    def allowed(self, module, include):
        source = ROOT / module / 'src/probe.cpp'
        checker = DepsChecker(str(ROOT))
        rules = checker.GetDirectoryRules(str(source.parent))
        cpp = CppChecker(False, True, str(ROOT))
        found, violation = cpp.CheckLine(rules, include, str(source))
        self.assertTrue(found)
        return violation is None

    def test_portable_layers_reject_outer_dependencies(self):
        for module, include in [
            ('domain', '<QtCore/QObject>'), ('domain', '"QObject"'),
            ('domain', '<application/graph_lifecycle.h>'),
            ('application', '<dataflow_runtime/runtime.h>'),
            ('libs/signal_contracts', '<domain/graph_definition.h>'),
            ('domain', '"../../presentation/qt/x.h"'),
            ('libs/dataflow_runtime', '<libs/bdsp_protocol/include/bdsp_protocol/numeric_data_codec.h>'),
            ('libs/dsp_kernels', '<../../dataflow_runtime/include/dataflow_runtime/runtime.h>'),
            ('domain', '<freertos/FreeRTOS.h>'),
            ('application', '<driver/i2s_std.h>'),
        ]:
            with self.subTest(module=module, include=include):
                self.assertFalse(self.allowed(module, '#include ' + include))

    def test_declared_inner_dependencies_remain_available(self):
        for module, include in [
            ('domain', '<dsp_kernels/window_math.h>'),
            ('application', '<domain/graph_definition.h>'),
            ('libs/dsp_kernels', '<signal_contracts/frame.h>'),
            ('libs/dsp_kernels', '<libs/signal_contracts/include/signal_contracts/frame.h>'),
        ]:
            with self.subTest(module=module, include=include):
                self.assertTrue(self.allowed(module, '#include ' + include))

    def test_tracy_is_confined_to_desktop_adapter(self):
        include = '#include <tracy/TracyC.h>'
        self.assertTrue(self.allowed('adapters/desktop', include))
        for module in ('domain', 'application', 'libs/dsp_kernels', 'libs/dataflow_runtime', 'presentation/qt'):
            with self.subTest(module=module):
                self.assertFalse(self.allowed(module, include))

    def test_c_and_header_fragments_are_not_skipped(self):
        for extension in ('.c', '.cpp', '.hpp', '.inc', '.inl'):
            self.assertTrue(CppChecker.IsCppFile('probe' + extension))


if __name__ == '__main__':
    unittest.main()
