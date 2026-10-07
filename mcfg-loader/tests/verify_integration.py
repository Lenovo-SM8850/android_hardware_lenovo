#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Standalone AIDL/app/native/policy/init/VINTF checks, with no ROM build.

Existing header jars, host tools and a monolithic neverallow configuration are
read-only inputs. Every intermediate/output goes under --scratch. The policy
check uses strict non-debug expansion; debug router startup and Treble policy
splitting still require the real Android build and enforcing device probes.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--android-source", type=Path, required=True)
parser.add_argument("--device", type=Path, required=True)
parser.add_argument("--scratch", type=Path, required=True)
parser.add_argument("--platform-header-jar", type=Path, required=True)
parser.add_argument("--tools-dir", type=Path, required=True)
parser.add_argument("--policy-conf", type=Path, required=True)
args = parser.parse_args()
root = args.android_source.resolve()
device = args.device.resolve()
out = args.scratch.resolve()
core = Path(__file__).resolve().parent.parent
aidl = core / "aidl"
name = "vendor.lenovo.hardware.mcfg"
if out == root or out.is_relative_to(root):
    parser.error("--scratch must be outside the Android source tree")
for directory in ["tools", "ndk", "include", "java", "classes", "api", "res", "gen", "policy"]:
    (out / directory).mkdir(parents=True, exist_ok=True)
for tool in ["aapt2", "checkpolicy", "checkfc", "checkseapp", "host_init_verifier", "assemble_vintf"]:
    shutil.copy2(args.tools_dir / tool, out / "tools" / tool)
shutil.copy2(args.platform_header_jar, out / "framework.jar")
shutil.copy2(args.policy_conf, out / "policy/base.conf")
environment = os.environ.copy()
environment["LD_LIBRARY_PATH"] = str(args.tools_dir.resolve().parent / "lib64")


def run(command, capture=False):
    command = list(map(str, command))
    print("COMMAND: " + " ".join(command), flush=True)
    return subprocess.run(command, check=True, env=environment,
                          stdout=subprocess.PIPE if capture else None).stdout


compiler = root / "prebuilts/build-tools/linux-x86/bin/aidl"
files = sorted((aidl / "vendor/lenovo/hardware/mcfg").glob("*.aidl"))
run([compiler, "--dumpapi", "--structured", "--stability=vintf", "-I" + str(aidl), "--out=" + str(out / "api"), *files])
api = aidl / "aidl_api" / name
for checked_in in [api / "2", api / "current"]:
    run([compiler, "--stability=vintf", "--checkapi=equal", checked_in, out / "api"])
hash_input = "".join(hashlib.sha1(p.read_bytes()).hexdigest() + "  ./" + p.relative_to(api / "2").as_posix() + "\n"
                     for p in sorted((api / "2").rglob("*.aidl"))) + "1\n"
digest = hashlib.sha1(hash_input.encode()).hexdigest()
if digest != (api / "2/.hash").read_text().strip():
    raise ValueError("frozen AIDL hash mismatch")
for language in ["java", "ndk"]:
    command = [compiler, "--lang=" + language, "--structured", "--stability=vintf", "--version=2", "--hash=" + digest,
               "-I" + str(aidl), "--out=" + str(out / language)]
    if language == "ndk":
        command.append("--header_out=" + str(out / "include"))
    run(command + files)
clang = root / "prebuilts/clang/host/linux-x86/clang-r584948b/bin/clang++"
includes = [out / "include", root / "frameworks/native/libs/binder/ndk/include_ndk",
            root / "frameworks/native/libs/binder/ndk/include_cpp", root / "frameworks/native/libs/binder/ndk/include_platform",
            root / "system/libbase/include", root / "external/jsoncpp/include", root / "external/selinux/libselinux/include"]
run([clang, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-D__INTRODUCED_IN(x)=", "-Wno-nullability-completeness",
     *["-I" + str(p) for p in includes], "-c", core / "service.cpp", "-o", out / "service.o"])
policy = device / "sepolicy"
fragments = [policy / "public/attributes", policy / "public/service.te", policy / "vendor/file.te", policy / "vendor/property.te",
             policy / "private/lenovo_mcfg_coordinator.te", policy / "public/hal_lenovo_mcfg.te", policy / "vendor/vendor_lenovo_mcfg_loader.te"]
expanded = run([root / "prebuilts/build-tools/linux-x86/bin/m4", "-Dtarget_build_variant=user", "-Dtarget_full_treble=true",
                "-Dtarget_compatible_property=true", "-Dtarget_exclude_build_test=false", root / "system/sepolicy/public/global_macros",
                root / "system/sepolicy/public/neverallow_macros", root / "system/sepolicy/public/te_macros", *fragments], capture=True).decode()
base = (out / "policy/base.conf").read_text()
position = base.index("role r;")
(out / "policy/combined.conf").write_text(base[:position] + expanded + "\n" + base[position:])
run([out / "tools/checkpolicy", "-M", "-c", "30", "-o", out / "policy/policy", out / "policy/combined.conf"])
for option, context in [(None, "file_contexts"), ("-s", "service_contexts"), ("-p", "property_contexts")]:
    run([out / "tools/checkfc", *([option] if option else []), out / "policy/policy", policy / "vendor" / context])
run([out / "tools/checkseapp", "-p", out / "policy/policy", "-o", out / "policy/seapp_contexts", policy / "private/seapp_contexts"])
run([out / "tools/host_init_verifier", device / "rootdir/etc/init.lenovo.mcfg-loader.rc"])
for xml in ["manifest_mcfg_loader.xml", "framework_matrix_mcfg_loader.xml"]:
    run([out / "tools/assemble_vintf", "-i", device / "vintf" / xml, "-o", out / xml])
print("PASS: frozen AIDL, native object, strict policy/context checks, init rc, VINTF XML")
print("NOT VERIFIED: Android target link/Soong, debug router policy, Treble policy split, runtime/device transport and modem behavior")
