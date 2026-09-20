#!/usr/bin/env python3
"""Exercise actual event-wait RAII and fence rechecks with a mock KMT/Win32."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
source = (here.parent / 'tu_knl_wddm.cc').read_text()
start = source.index('class tu_wddm_fence_event_wait {')
end = source.index('\nbool\ntu_wddm_context_wait_submissions', start)
production = source[start:end]
fixture = (here / 'tu_wddm_fence_event_test.cpp').read_text().replace('// PRODUCTION', production)
controls = {
    'production': fixture,
    'missing-cancel': fixture.replace('if (!escape(registrations[i].context, &request))', 'if (false)'),
    'premature-success': fixture.replace('if (completed == fence || tu_wddm_fence_after(completed, fence))', 'if (true)'),
    'lost-reset-check': fixture.replace('if (!tu_wddm_device_execution_active(context->device))', 'if (false)'),
    'clear-inline-signal': fixture.replace('DWORD result = WaitForSingleObject(event, milliseconds);', 'ResetEvent(event); DWORD result = WaitForSingleObject(event, milliseconds);'),
}
with tempfile.TemporaryDirectory(prefix='wddm-event-wait-') as temporary:
    out = Path(temporary)
    for name, text in controls.items():
        if name != 'production':
            assert text != fixture
        (out / 'test.cpp').write_text(text)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-unused-but-set-variable',
                        '-fsanitize=address,undefined', '-I' + str(here.parent),
                        str(out / 'test.cpp'), '-o', str(out / 'test')], check=True)
        result = subprocess.run([str(out / 'test')], capture_output=True, text=True)
        print(name, result.returncode, result.stdout.strip(), result.stderr[:400])
        assert result.returncode == (0 if name == 'production' else 1)
        if name != 'production':
            assert 'FAIL' in result.stdout and 'Sanitizer' not in result.stderr
