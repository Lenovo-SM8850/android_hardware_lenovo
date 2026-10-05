#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile and execute the production ownership state without Android or a device."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="dolby-color-tests-") as classes:
    subprocess.run(["javac", "-d", classes,
                    str(root / "src/io/github/miner7222/dolbyvision/ColorModeOverride.java"),
                    str(root / "tests/ColorModeOverrideTest.java")], check=True)
    subprocess.run(["java", "-ea", "-cp", classes,
                    "io.github.miner7222.dolbyvision.ColorModeOverrideTest"], check=True)
