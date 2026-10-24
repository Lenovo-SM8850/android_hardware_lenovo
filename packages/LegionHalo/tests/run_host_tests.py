#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise production capture/tracking lifecycles with a deterministic Android clock."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from host_stubs import STUBS

here = Path(__file__).resolve().parent
root = here.parent
top = Path(os.environ.get("ANDROID_BUILD_TOP", Path.home() / "derp"))
env = dict(os.environ)
jdk = top / "prebuilts/jdk/jdk21/linux-x86"
if jdk.is_dir():
    env["JAVA_HOME"] = str(jdk)
compiler = os.environ.get("KOTLINC") or shutil.which("kotlinc") or str(top / "external/kotlinc/bin/kotlinc")
java = str(Path(env["JAVA_HOME"]) / "bin/java") if "JAVA_HOME" in env else "java"
with tempfile.TemporaryDirectory(prefix="halo-lifecycle-") as directory:
    target = Path(directory)
    sources = []
    for name, content in STUBS.items():
        path = target / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        sources.append(str(path))
    sources.extend(str(root / "src/io/github/miner7222/halo" / name)
                   for name in ["AudioPulseWorker.kt", "ReactiveSession.kt"])
    sources.append(str(here / "ReactiveLifecycleTest.kt"))
    subprocess.run([compiler, "-nowarn", *sources, "-include-runtime", "-d", str(target / "test.jar")],
                   check=True, env=env)
    subprocess.run([java, "-jar", str(target / "test.jar")], check=True)
