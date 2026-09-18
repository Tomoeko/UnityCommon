#!/usr/bin/env python3
"""Run only the tiny isolated exact-Editor content fixture and remove its scratch."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time


CAP_SCRATCH = 512 * 1024 * 1024
CAP_RETAINED = 4 * 1024 * 1024
CAP_SECONDS = 240


def tree_size(root):
    total = 0
    for current, directories, files in os.walk(root, followlinks=False):
        directories[:] = [name for name in directories if not Path(current, name).is_symlink()]
        for name in files:
            path = Path(current, name)
            try:
                if not path.is_symlink():
                    total += path.stat().st_size
            except FileNotFoundError:
                pass
    return total


def terminate_owned_group(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)


def run(root, output, editor, log_cap=64 * 1024):
    target = 'StandaloneWindows64'
    output = output.resolve()
    with editor.open('rb') as stream:
        editor_sha256 = hashlib.file_digest(stream, 'sha256').hexdigest()
    if output.exists():
        raise RuntimeError('Refusing to overwrite an existing fixture receipt: ' + str(output))
    output.mkdir()
    scratch = Path(tempfile.mkdtemp(prefix='UnityCommon-fixture-'))
    log = output / 'editor.log'
    report = {'target': target, 'editor_sha256': editor_sha256,
              'scratch_cap_bytes': CAP_SCRATCH,
              'retained_cap_bytes': CAP_RETAINED, 'log_cap_bytes': log_cap,
              'wall_cap_seconds': CAP_SECONDS, 'peak_scratch_bytes': 0,
              'peak_retained_bytes': 0, 'scratch_removed': False}
    process = None
    started = time.monotonic()
    try:
        for directory in ('Assets/Editor', 'Packages', 'ProjectSettings', 'FixtureCompiled'):
            (scratch / directory).mkdir(parents=True)
        (scratch / 'Packages/manifest.json').write_text('{"dependencies":{}}\n')
        (scratch / 'ProjectSettings/ProjectVersion.txt').write_text(
            'm_EditorVersion: 2021.3.35f1\n'
            'm_EditorVersionWithRevision: 2021.3.35f1 (157b46ce122a)\n')
        # Preserved .meta identities keep dependency order and script PathIDs stable.
        shutil.copytree(root / 'inputs/Assets', scratch / 'Assets', dirs_exist_ok=True)
        environment = os.environ.copy()
        environment['UNITYCOMMON_FIXTURE_OUTPUT'] = str(output)
        environment['UNITYCOMMON_FIXTURE_TARGET'] = target
        command = [str(editor), '-batchmode', '-nographics', '-projectPath', str(scratch),
                   '-executeMethod', 'DirectoryFixtureBuilder.Run', '-quit', '-logFile', str(log)]
        report['command'] = ['Unity', '-batchmode', '-nographics', '-projectPath',
                             '<scratch>', '-executeMethod', 'DirectoryFixtureBuilder.Run',
                             '-quit', '-logFile', 'editor.log']
        process = subprocess.Popen(command, env=environment, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, start_new_session=True)
        while process.poll() is None:
            scratch_bytes = tree_size(scratch)
            retained_bytes = tree_size(output)
            report['peak_scratch_bytes'] = max(report['peak_scratch_bytes'], scratch_bytes)
            report['peak_retained_bytes'] = max(report['peak_retained_bytes'], retained_bytes)
            if scratch_bytes > CAP_SCRATCH:
                raise RuntimeError('Scratch byte cap exceeded')
            if retained_bytes > CAP_RETAINED:
                raise RuntimeError('Retained byte cap exceeded')
            if log.exists() and log.stat().st_size > log_cap:
                raise RuntimeError('Editor log byte cap exceeded')
            if time.monotonic() - started > CAP_SECONDS:
                raise RuntimeError('Editor wall-time cap exceeded')
            time.sleep(1)
        report['exit_code'] = process.returncode
        report['status'] = 'completed'
    except Exception as error:
        report['status'] = 'bounded_failure'
        report['error'] = str(error)
    finally:
        if process is not None:
            terminate_owned_group(process)
            report.setdefault('exit_code', process.returncode)
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        report['final_scratch_bytes'] = tree_size(scratch)
        report['final_retained_before_log_trim_bytes'] = tree_size(output)
        report['full_log_bytes'] = log.stat().st_size if log.exists() else 0
        if log.exists() and log.stat().st_size > log_cap:
            # Shutdown can grow the log after the last sample. Read only its
            # retained tail, rather than allocating the complete final log.
            with log.open('rb') as stream:
                stream.seek(-log_cap, os.SEEK_END)
                tail = stream.read(log_cap)
            log.write_bytes(tail)
            report['log_retained_tail_only'] = True
        shutil.rmtree(scratch)
        report['scratch_removed'] = not scratch.exists()
        pins = []
        for path in sorted(output.rglob('*')):
            if path.is_file():
                with path.open('rb') as stream:
                    digest = hashlib.file_digest(stream, 'sha256').hexdigest()
                pins.append({'path': str(path.relative_to(output)), 'bytes': path.stat().st_size,
                             'sha256': digest})
        report['retained_files'] = pins
        (output / 'runner-report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2), flush=True)
    return 0 if report.get('exit_code') == 0 and report['status'] == 'completed' else 1


def main(root, log_cap=64 * 1024):
    parser = argparse.ArgumentParser(description="Generate isolated Unity 2021.3.35f1 fixtures")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--unity", type=Path,
                        default=Path(os.environ.get("UNITY_EDITOR_PATH",
                            "/Applications/Unity/Unity.app/Contents/MacOS/Unity")))
    arguments = parser.parse_args()
    return run(root, arguments.output, arguments.unity.resolve(), log_cap)
