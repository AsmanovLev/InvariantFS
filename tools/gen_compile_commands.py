#!/usr/bin/env python3
import json
import os
import glob

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CC = "/usr/bin/cc"

CFLAGS = [
    "-std=gnu11",
    "-O2",
    "-I" + os.path.join(REPO, "src"),
    "-I" + os.path.join(REPO, "src", "core"),
    "-I" + os.path.join(REPO, "src", "codecs"),
    "-I" + os.path.join(REPO, "src", "recipes"),
    "-I" + os.path.join(REPO, "src", "cli"),
    "-I" + os.path.join(REPO, "src", "vendor7z"),
    "-I" + os.path.join(REPO, "src", "legacy"),
    "-I" + os.path.join(REPO, "tools"),
    "-I/usr/include/fuse3",
    "-pthread",
    "-DINVFS_EMBED_FLACX",
    "-DMINIZ_NO_ZLIB_APIS",
    "-DBLAKE3_NO_SSE2",
    "-DBLAKE3_NO_SSE41",
    "-DBLAKE3_NO_AVX2",
    "-DBLAKE3_NO_AVX512",
    '-DINVFS_VERSION_STRING="v0.5.0"',
    '-DINVFS_BUILD_DATE="2026-09-23"',
    '-DINVFS_AUTHOR_NAME="Lev_Asmanov"',
    '-DINVFS_LICENSE="GPL-2.0-only"'
]

commands = []

for root, _, files in os.walk(os.path.join(REPO, "src")):
    for f in files:
        if f.endswith(".c"):
            src_file = os.path.join(root, f)
            obj_file = os.path.join(REPO, "build", "obj", os.path.splitext(f)[0] + ".o")
            cmd = [CC] + CFLAGS + ["-c", src_file, "-o", obj_file]
            commands.append({
                "directory": REPO,
                "command": " ".join(cmd),
                "file": src_file
            })

for root, _, files in os.walk(os.path.join(REPO, "tools")):
    for f in files:
        if f.endswith(".c"):
            src_file = os.path.join(root, f)
            cmd = [CC] + CFLAGS + ["-c", src_file]
            commands.append({
                "directory": REPO,
                "command": " ".join(cmd),
                "file": src_file
            })

out_path = os.path.join(REPO, "compile_commands.json")
with open(out_path, "w") as fp:
    json.dump(commands, fp, indent=2)

print(f"Generated {out_path} with {len(commands)} entries.")
