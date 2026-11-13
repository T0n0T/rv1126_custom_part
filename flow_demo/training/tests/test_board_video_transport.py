from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[2] / 'board/run_rknn_video_demo.sh'


@pytest.fixture
def transport(tmp_path: Path) -> tuple[dict[str, str], Path]:
    adb = tmp_path / 'adb'
    adb.write_text(
        f'#!{sys.executable}\n'
        'import json, os, sys, time\n'
        'from pathlib import Path\n'
        'Path(os.environ["ADB_CALLS"]).write_text(json.dumps(sys.argv[1:]))\n'
        'mode = os.environ["ADB_TEST_MODE"]\n'
        'if mode == "hang": time.sleep(10)\n'
        'if mode == "offline":\n'
        '    print("adb: device offline", file=sys.stderr)\n'
        '    sys.exit(1)\n'
        'print("Rockchip RV1126B XIAOYU 50IPC V10 Board" if mode == "ready" else "Other board")\n'
    )
    adb.chmod(0o755)
    python = tmp_path / 'python'
    python.write_text('#!/bin/sh\nprintf called > "$PREPROCESS_CALLED"\nexit 91\n')
    python.chmod(0o755)
    video = tmp_path / 'video.mp4'
    video.touch()
    model = tmp_path / 'model.rknn'
    model.touch()
    env = {
        **os.environ,
        'ADB': str(adb),
        'ADB_SERIAL': '192.168.1.63:5555',
        'ADB_SERVER_PORT': '5038',
        'ADB_TIMEOUT': '2',
        'ADB_PREFLIGHT_TIMEOUT': '1',
        'ADB_CALLS': str(tmp_path / 'adb-calls.json'),
        'PYTHON': str(python),
        'PREPROCESS_CALLED': str(tmp_path / 'preprocess-called'),
        'MODEL': str(model),
        'INFER_BIN': '/bin/true',
        'OUTPUT_ROOT': str(tmp_path / 'runs'),
    }
    return env, video


@pytest.mark.parametrize('mode', ['offline', 'hang', 'wrong-board'])
def test_unusable_board_fails_before_preprocessing(transport, mode: str) -> None:
    env, video = transport
    started = time.monotonic()
    result = subprocess.run(
        ['sh', str(SCRIPT), str(video), mode],
        env={**env, 'ADB_TEST_MODE': mode}, capture_output=True, text=True, timeout=5,
    )
    assert result.returncode != 0
    assert not Path(env['PREPROCESS_CALLED']).exists()
    assert time.monotonic() - started < 4
    calls = json.loads(Path(env['ADB_CALLS']).read_text())
    assert calls[:5] == ['-P', '5038', '-s', '192.168.1.63:5555', 'shell']
    log = Path(env['OUTPUT_ROOT']) / mode / 'adb-preflight.log'
    assert log.is_file()
    assert 'preflight' in result.stderr.lower()
    if mode == 'hang':
        assert 'timed out' in log.read_text().lower()


def test_usable_board_allows_preprocessing(transport) -> None:
    env, video = transport
    result = subprocess.run(
        ['sh', str(SCRIPT), str(video), 'ready'],
        env={**env, 'ADB_TEST_MODE': 'ready'}, capture_output=True, text=True, timeout=5,
    )
    assert result.returncode == 91  # Deliberate stop at the preprocessing boundary.
    assert Path(env['PREPROCESS_CALLED']).exists()
    calls = json.loads(Path(env['ADB_CALLS']).read_text())
    assert calls[:5] == ['-P', '5038', '-s', '192.168.1.63:5555', 'shell']
    log = Path(env['OUTPUT_ROOT']) / 'ready' / 'adb-preflight.log'
    assert 'RV1126B' in log.read_text()
