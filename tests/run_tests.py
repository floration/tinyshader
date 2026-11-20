#!/usr/bin/env python3
"""Compile every test shader with tinyshader and DXC, then validate the SPIR-V.

Needs dxc, spirv-val and spirv-opt on PATH (all ship with the Vulkan SDK).
Usage: python tests/run_tests.py [--no-dxc] [--no-opt] [filter]
"""

import os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
os.chdir(ROOT)

args = [a for a in sys.argv[1:] if not a.startswith("--")]
use_dxc = "--no-dxc" not in sys.argv
use_opt = "--no-opt" not in sys.argv
name_filter = args[0] if args else ""

for tool in (["spirv-val"] + (["dxc"] if use_dxc else []) + (["spirv-opt"] if use_opt else [])):
    if not shutil.which(tool):
        sys.exit(f"error: '{tool}' not found on PATH (install the Vulkan SDK)")

subprocess.run(["cmake", "-B", "build", "-DCMAKE_BUILD_TYPE=Release"], check=True,
               stdout=subprocess.DEVNULL)
subprocess.run(["cmake", "--build", "build", "--config", "Release"], check=True,
               stdout=subprocess.DEVNULL)

compiler_exe = None
for cand in ["build/tsc", "build/tsc.exe", "build/Release/tsc.exe", "build/Debug/tsc.exe"]:
    if os.path.exists(cand):
        compiler_exe = cand
        break
if not compiler_exe:
    sys.exit("error: could not find the built tsc executable")

STAGES = {".frag": ("fragment", "ps_6_0"), ".vert": ("vertex", "vs_6_0"), ".comp": ("compute", "cs_6_0")}
TARGET_ENV = "vulkan1.1"

def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return r.returncode == 0, r.stdout.strip()

failed = []
sizes = []

def test_dir(dir, expect_ok):
    print(f"\n=== {'VALID' if expect_ok else 'INVALID'} tests ===")
    outdir = dir + "_out"
    os.makedirs(outdir, exist_ok=True)

    for filename in sorted(os.listdir(dir)):
        base, ext = os.path.splitext(filename)
        if ext != ".hlsl" or name_filter not in filename:
            continue
        stage_ext = os.path.splitext(base)[1]
        if stage_ext not in STAGES:
            continue
        stage, dxc_profile = STAGES[stage_ext]
        src = os.path.join(dir, filename)
        out = os.path.join(outdir, base + ".spv")
        problems = []

        ok, log = run([compiler_exe, "-T", stage, "-E", "main", "-o", out, src])
        if ok != expect_ok:
            problems.append(("tinyshader " + ("failed" if expect_ok else "accepted invalid shader"), log))

        if ok and expect_ok:
            ok_val, log = run(["spirv-val", "--target-env", TARGET_ENV, out])
            if not ok_val:
                problems.append(("spirv-val rejected tinyshader output", log))
            elif use_opt:
                opt_out = os.path.join(outdir, base + ".opt.spv")
                ok_opt, log = run(["spirv-opt", "-O", "--target-env=" + TARGET_ENV, out, "-o", opt_out])
                if not ok_opt:
                    problems.append(("spirv-opt -O failed", log))
                else:
                    sizes.append((filename, os.path.getsize(out), os.path.getsize(opt_out)))

        if use_dxc:
            dxc_out = os.path.join(outdir, base + ".dxc.spv")
            ok_dxc, log = run(["dxc", src, "-E", "main", "-T", dxc_profile, "-spirv",
                               "-fspv-target-env=" + TARGET_ENV, "-Fo", dxc_out])
            if ok_dxc != expect_ok:
                # Disagreement with DXC is worth knowing about, but it is the
                # reference, so report it separately rather than as our failure.
                problems.append(("DXC disagrees (" + ("rejects" if expect_ok else "accepts") + ")", log))

        status = "FAIL" if problems else "ok"
        print(f"  [{status:>4}] {src}")
        for what, log in problems:
            print(f"         {what}")
            for line in log.splitlines()[:12]:
                print("           " + line)
        if problems:
            failed.append(src)

test_dir("tests/valid", True)
test_dir("tests/invalid", False)

if sizes:
    print("\n=== SPIR-V size (bytes): tinyshader -> after spirv-opt -O ===")
    for name, raw, opt in sizes:
        print(f"  {name:40} {raw:6} -> {opt:6}")

exe_size = os.path.getsize(compiler_exe)
print(f"\ntsc binary: {exe_size/1024:.0f} KiB ({compiler_exe})")

print("\n=== RESULTS ===")
if failed:
    print(f"{len(failed)} failed:")
    for f in failed:
        print("  " + f)
    sys.exit(1)
print("All tests passed")
