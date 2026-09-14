# SPDX-License-Identifier: MIT
"""Bounded cleanup retry for compiled fixtures; never suppress test failures."""
from contextlib import contextmanager
from pathlib import Path
import shutil
import tempfile
import time


# Windows can transiently retain an executable after its process has exited.
@contextmanager
def fixture_directory():
    directory = tempfile.mkdtemp(prefix='zink-cache-')
    try:
        yield Path(directory)
    finally:
        for attempt in range(8):
            try:
                shutil.rmtree(directory)
                break
            except PermissionError:
                if attempt == 7:
                    raise
                time.sleep(0.05 * (2 ** attempt))
