#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build and execute the production core on Linux, outside any ROM output tree."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import importlib.util
import json
import os

parser = argparse.ArgumentParser()
parser.add_argument("--android-source", type=Path, required=True)
parser.add_argument("--payload", type=Path, required=True)
parser.add_argument("--scratch", type=Path, required=True)
parser.add_argument("--nezha", type=Path)
parser.add_argument("--generator", type=Path)
args = parser.parse_args()
args.scratch = args.scratch.resolve()
args.android_source = args.android_source.resolve()
if args.scratch == args.android_source or args.scratch.is_relative_to(args.android_source):
    parser.error("--scratch must be outside the Android source tree")
core = Path(__file__).resolve().parent.parent
jsoncpp = args.android_source / "external/jsoncpp"
args.scratch.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="mcfg-test-", dir=args.scratch) as directory:
    # Exercise the production dlopen/callback/timeout/reopen path without a modem.
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", "-fPIC", "-shared", "-DMCFG_FAKE_DIAG",
                    str(core / "tests/host_test.cpp"), "-o", directory + "/libdiag.so"], check=True)
    environment = os.environ.copy()
    environment["LD_LIBRARY_PATH"] = directory + ":" + environment.get("LD_LIBRARY_PATH", "")
    environment["MCFG_FAKE_STATE"] = directory + "/diag-state"
    binary = Path(directory) / "host_test"
    command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I" + str(core),
               "-I" + str(jsoncpp / "include"), str(core / "ProfileStore.cpp"),
               str(core / "MbnParser.cpp"),
               str(core / "Transaction.cpp"), str(core / "FirmwareGeneration.cpp"), str(core / "Recovery.cpp"), str(core / "EfsClient.cpp"),
               str(core / "DiagTransport.cpp"), str(core / "tests/host_test.cpp"),
               *map(str, sorted((jsoncpp / "src/lib_json").glob("*.cpp"))),
               "-lcrypto", "-lz", "-ldl", "-pthread", "-o", str(binary)]
    print("BUILD: " + " ".join(command), flush=True)
    subprocess.run(command, check=True)
    subprocess.run([str(binary), "--fake-diag-cache"], env=environment, check=True)
    Path(environment["MCFG_FAKE_STATE"]).unlink()
    result = subprocess.run([str(binary), str(args.payload), directory + "/cases"],
                            env=environment, timeout=180, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    print(result.stdout, end="", flush=True)
    result.check_returncode()
    if "PASS: exec reconnect resets libdiag cache;" not in result.stdout:
        raise ValueError("exec reconnect regression was not exercised")
    if "PASS: multiple imports per carrier," not in result.stdout:
        raise ValueError("import selection and migration regression was not exercised")
    if "PASS: single Binder thread stale lease fails promptly;" not in result.stdout:
        raise ValueError("single-threaded readiness regression was not exercised")
    if result.stdout.count("host repeated transport error") != 1:
        raise ValueError("identical transport errors were not rate-limited")
    print("PASS: 360 identical transport diagnostics emitted one log", flush=True)
    restart = Path(directory) / "modem_restart_test"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", "-pthread",
                    str(core / "tests/modem_restart_test.cpp"), "-o", str(restart)], check=True)
    subprocess.run([str(restart), directory + "/sysfs"], check=True)
    if args.nezha:
        if not args.generator:
            parser.error("--nezha needs --generator (original read-only build_profile.py)")
        spec = importlib.util.spec_from_file_location("build_profile", args.generator)
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        mbn_root = args.nezha / "mbn/nezha/image/modem_pr/mcfg/configs/mcfg_sw/generic"
        donors = ["Korea/KT/Commercial_KT_LTE", "Korea/SKT/Commercial", "Korea/LGU/Commercial",
                  "NA/TMO/Commercial", "SEA/StarHub/Commercial/SG", "Europe/Vodafone/Commercial"]
        # Select additional actual generated donors if a firmware uses other directory names.
        inventory = json.loads((args.nezha / "inventory.json").read_bytes())
        candidates = [row["profile_id"] for row in inventory["nezha_sw"] if row.get("status") == "generated_validated"]
        donors = [d for d in donors if (mbn_root / d / "unpacked").is_dir()]
        for candidate in candidates:
            if len(donors) >= 6:
                break
            if candidate not in donors:
                donors.append(candidate)
        for n, donor in enumerate(donors):
            original = mbn_root / donor / "mcfg_sw.mbn"
            result = generator.build_profile(mbn_root / donor / "unpacked", "parity" + str(n),
                profiles_root=Path(directory) / "python", allowlist_path=args.nezha / "tools/writable_efs_paths.json")
            manifest = json.loads((result["destination"] / "profile.json").read_bytes())
            expected = {item["remote_path"]: (result["destination"] / item["file"]).read_bytes().hex()
                        for item in manifest["items"]}
            output = Path(directory) / ("parsed" + str(n) + ".json")
            subprocess.run([str(binary), "--parse", str(args.payload), str(original),
                            directory + "/imports" + str(n), str(output)], check=True)
            if json.loads(output.read_bytes()) != expected:
                raise ValueError("parser parity mismatch: " + donor)
            print(f"PARITY PASS: {donor}: {len(expected)} byte-identical EFS payloads", flush=True)
        invalid = Path(directory) / "truncated.mbn"
        invalid.write_bytes(original.read_bytes()[:64])
        result = subprocess.run([str(binary), "--parse", str(args.payload), str(invalid),
                                 directory + "/invalid", str(output)], capture_output=True)
        if result.returncode == 0:
            raise ValueError("truncated MBN accepted")
        print("REJECTION PASS: truncated MBN: " + result.stderr.decode().strip(), flush=True)
        print("PASS: parser parity, durable imported choice, aliases, mixed-carrier refusal and deletion", flush=True)
    classes = Path(directory) / "java"
    classes.mkdir()
    gate = core.parent / "packages/McfgLoader/src/io/github/miner7222/mcfg/CoordinatorState.java"
    subprocess.run(["javac", "-d", str(classes), str(gate), str(core / "tests/CoordinatorStateTest.java")], check=True)
    subprocess.run(["java", "-ea", "-cp", str(classes), "io.github.miner7222.mcfg.CoordinatorStateTest"], check=True)
