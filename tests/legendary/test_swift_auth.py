"""Compile and run ios/Auth/AuthCodeExtractor.swift (Foundation only) with a Swift toolchain.

LoginView.swift needs SwiftUI/WebKit, which do not exist on Linux, so only its syntax is
checked here (``swiftc -parse``); the extractor holding the URL/JSON logic is built and run.
Skipped when no Swift compiler is found.  Point SWIFTC at one (and SWIFT_LD_LIBRARY_PATH at
its libraries, including <root>/usr/lib/swift/linux, if it is not installed system-wide), e.g. a Debian ``swiftlang`` package
unpacked with ``dpkg -x``.
"""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
AUTH = REPO / "ios" / "Auth"
SWIFTC = os.environ.get("SWIFTC") or shutil.which("swiftc")

pytestmark = pytest.mark.skipif(not SWIFTC, reason="no swiftc (set SWIFTC)")


def _env():
    env = dict(os.environ)
    if os.environ.get("SWIFT_LD_LIBRARY_PATH"):
        env["LD_LIBRARY_PATH"] = os.environ["SWIFT_LD_LIBRARY_PATH"]
    return env


def test_swift_files_parse():
    subprocess.run([SWIFTC, "-parse", str(AUTH / "LoginView.swift"), str(AUTH / "AuthCodeExtractor.swift")],
                   check=True, env=_env(), capture_output=True, timeout=300)


def test_auth_code_extractor_runs(tmp_path):
    exe = tmp_path / "extractor_test"
    subprocess.run([SWIFTC, "-O", str(AUTH / "AuthCodeExtractor.swift"),
                    str(Path(__file__).parent / "swift" / "main.swift"), "-o", str(exe)],
                   check=True, env=_env(), capture_output=True, timeout=600, cwd=tmp_path)
    out = subprocess.run([str(exe)], env=_env(), capture_output=True, text=True, timeout=60)
    assert out.returncode == 0, out.stdout + out.stderr
    assert "ALL PASSED" in out.stdout
