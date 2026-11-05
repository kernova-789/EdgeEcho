#!/usr/bin/env python3
"""Meaningful failure-path checks for the validation gate, no external compiler."""
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
from validation_support import assert_sources_unchanged, assert_test_inventory, executable_identity, junit_result, source_id


class ValidationGateTests(unittest.TestCase):
    def setUp(self):
        # Every temporary file is still inside EdgeEcho, including CI runs.
        self.output = ROOT/'build-validation-tests'
        self.output.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=self.output)
        self.folder = Path(self.temporary.name)
        self.start = time.time_ns()

    def tearDown(self):
        self.temporary.cleanup()

    def result(self, cases):
        path = self.folder/'junit.xml'
        path.write_text('<testsuites><testsuite>' + cases + '</testsuite></testsuites>')
        return path

    def test_real_complete_inventory(self):
        result = junit_result(self.result('<testcase name="core"/><testcase name="gui"/>'),
                              ['core','gui'], started_ns=self.start)
        self.assertEqual(result['passed'], 2)

    def test_skipped_failed_and_disabled_cannot_pass(self):
        for content in ('<testcase name="core"><skipped/></testcase>',
                        '<testcase name="core"><failure/></testcase>',
                        '<testcase name="core"><error/></testcase>',
                        '<testcase name="core" status="notrun"/>'):
            with self.subTest(content=content), self.assertRaises(RuntimeError):
                junit_result(self.result(content), ['core'], started_ns=self.start)

    def test_empty_or_partial_inventory_cannot_pass(self):
        for cases, expected in (('', ['core']), ('<testcase name="core"/>', ['core','gui']),
                                ('<testcase name="core"/><testcase name="core"/>', ['core'])):
            with self.assertRaises(RuntimeError):
                junit_result(self.result(cases), expected, started_ns=self.start)

    def test_stale_or_missing_result_cannot_pass(self):
        path = self.result('<testcase name="core"/>')
        os.utime(path, (1, 1))
        with self.assertRaises(RuntimeError):
            junit_result(path, ['core'], started_ns=self.start)
        with self.assertRaises(RuntimeError):
            junit_result(self.folder/'old-LastTest.log', ['core'], started_ns=self.start)

    def test_source_changes_and_empty_fingerprints_rejected(self):
        assert_sources_unchanged({'a':'1'}, {'a':'1'})
        with self.assertRaises(RuntimeError):
            assert_sources_unchanged({'a':'1'}, {'a':'2'})
        with self.assertRaises(RuntimeError):
            assert_sources_unchanged({}, {})
        with self.assertRaises(RuntimeError):
            source_id({})

    def test_required_test_inventory_cannot_silently_shrink(self):
        core = ['database','codec_playback','session','export','browsing','validation_runner',
                'cli_demo','cli_inspect','cli_invalid_number']
        assert_test_inventory(core,gui=False)
        with self.assertRaises(RuntimeError):
            assert_test_inventory(core,gui=True)
        with self.assertRaises(RuntimeError):
            assert_test_inventory(core[:-1],gui=False)
        assert_test_inventory(core+['gui_smoke','gui_inputs','plot_widget','time_range_editor'],gui=True)

    def test_identity_requires_source_build(self):
        identity = 'source-sha256:' + 'a'*64
        self.assertEqual(executable_identity('EdgeEcho 0.3.0\nBuild: '+identity), ('0.3.0', identity))
        with self.assertRaises(RuntimeError):
            executable_identity('EdgeEcho 0.3.0')


if __name__ == '__main__':
    unittest.main()
