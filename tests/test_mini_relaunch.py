"""Check that dismissing a failed Mini notification releases the single-instance mutex."""
import subprocess
import sys
import time

exe = sys.argv[1]
for mode in ("--ui-close-failed-test", "--ui-close-pending-failed-test"):
    for attempt in range(2):
        start = time.monotonic()
        result = subprocess.run([exe, mode], capture_output=True, timeout=5)
        elapsed = time.monotonic() - start
        assert result.returncode == 0, (
            f"{mode} attempt {attempt + 1}: process exited with {result.returncode}"
        )
        assert elapsed < 3, (
            f"{mode} attempt {attempt + 1}: notification remained open for {elapsed:.2f}s"
        )
print("Failed and pending-failed Mini notification close/relaunch checks passed")
