# SPDX-License-Identifier: MIT
"""Verify cleanup retries do not turn failed tests or permanent locks into PASS."""
from unittest import TestCase, main
from unittest.mock import patch

from fixture_directory import fixture_directory


class CleanupTests(TestCase):
    # Mock directory creation too: tests never leave real directories behind.
    @patch('fixture_directory.tempfile.mkdtemp', return_value='fixture')
    @patch('fixture_directory.time.sleep')
    @patch('fixture_directory.shutil.rmtree', side_effect=[PermissionError(), None])
    def test_transient_retry(self, remove, sleep, create):
        with fixture_directory():
            pass
        self.assertEqual(remove.call_count, 2)
        sleep.assert_called_once_with(0.05)
        create.assert_called_once()

    @patch('fixture_directory.tempfile.mkdtemp', return_value='fixture')
    @patch('fixture_directory.time.sleep')
    @patch('fixture_directory.shutil.rmtree', side_effect=PermissionError())
    def test_permanent_failure(self, remove, sleep, create):
        with self.assertRaises(PermissionError), fixture_directory():
            pass
        self.assertEqual(remove.call_count, 8)
        self.assertEqual(sleep.call_count, 7)

    @patch('fixture_directory.tempfile.mkdtemp', return_value='fixture')
    @patch('fixture_directory.time.sleep')
    @patch('fixture_directory.shutil.rmtree', side_effect=[PermissionError(), None])
    def test_body_failure_is_preserved(self, remove, sleep, create):
        with self.assertRaisesRegex(RuntimeError, 'test failed'), fixture_directory():
            raise RuntimeError('test failed')
        self.assertEqual(remove.call_count, 2)


if __name__ == '__main__':
    main()
