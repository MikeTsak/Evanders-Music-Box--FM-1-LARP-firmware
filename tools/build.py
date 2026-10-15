#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build Evander's Music Box (BUILDING.md).

  python3 tools/build.py test                 host self-test (any C compiler, or zig)
  python3 tools/build.py web                  the wasm core and the site: build/web/
  python3 tools/build.py check                compile-check the FM-1 port without the JieLi toolchain
  python3 tools/build.py fw [--version 1.0]   the FM-1 firmware: build/fw/emb.fwsc (JieLi pi32v2 toolchain)
  python3 tools/build.py all [--version 1.0]  fw, then web with the package in it (the installer serves it)
  python3 tools/build.py wav SLOT SECONDS     render an Encounter scene to build/scene-N.wav
  python3 tools/build.py shots                screenshots into build/shots/

C compilers for test / web, the first found: $CC (host) / $WASM_CC (wasm), clang, `python3 -m ziglang cc`
(pip install ziglang: one self-contained download, Windows / macOS / Linux).
The firmware needs JIELI_TOOLCHAIN (tools: upstream/felucca/tools/get_toolchain.sh) and AC79_SDK, and on
anything but Linux x86-64, Docker (each tool runs in a linux/amd64 container, as Felucca's build does).
"""
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UP = ROOT / "upstream" / "felucca"
FW = UP / "firmware"
BUILD = ROOT / "build"
PRODUCT = "FM-1_777"                     # the package identity; the installer and the device compare it
DOCKER_IMAGE = os.environ.get("JIELI_DOCKER_IMAGE", "debian:bookworm-slim")
CFLAGS_FW = ["-Os", "-ffunction-sections", "-fno-builtin", "-Wall", "-Wno-unused-function"]


def need_upstream():
    if not (FW / "hal" / "fm1_audio.h").exists():
        raise SystemExit("upstream/felucca is empty: git submodule update --init")


def find_cc(env, wasm):
    if os.environ.get(env):
        return os.environ[env].split()
    if shutil.which("clang"):
        probe = subprocess.run(["clang", "-print-targets"], capture_output=True, text=True)
        if not wasm or "wasm32" in probe.stdout:
            return ["clang"]
    try:
        subprocess.run([sys.executable, "-m", "ziglang", "version"], capture_output=True, check=True)
        return [sys.executable, "-m", "ziglang", "cc"]
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    if not wasm and shutil.which("gcc"):
        return ["gcc"]
    raise SystemExit(f"build: no C compiler for {'wasm32' if wasm else 'the host'}: "
                     f"install clang, or `pip install ziglang`, or set {env}")


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode:
        raise SystemExit(f"build: {Path(str(cmd[0])).name} failed ({r.returncode})")
    return r


# ------------------------------------------------------------------ host

def host_exe():
    out = BUILD / "host" / ("emb_host.exe" if os.name == "nt" else "emb_host")
    out.parent.mkdir(parents=True, exist_ok=True)
    run([*find_cc("CC", False), "-std=c99", "-O2", "-Wall", "-Wextra", "-Wno-unused-function",
         "-Wno-unused-parameter", "-o", out, ROOT / "src" / "platform" / "host.c"])
    return out


def cmd_test(_a):
    run([host_exe()])


def cmd_wav(a):
    out = BUILD / f"scene-{a.slot + 1}.wav"
    run([host_exe(), "wav", a.slot, a.seconds, out])


def cmd_shots(_a):
    d = BUILD / "shots"
    d.mkdir(parents=True, exist_ok=True)
    run([host_exe(), "shots", d])
    try:                                            # PNGs too, when Pillow is there
        from PIL import Image
        for p in d.glob("*.ppm"):
            Image.open(p).resize((480, 480), Image.NEAREST).save(p.with_suffix(".png"))
    except ImportError:
        pass


# ------------------------------------------------------------------ web

def cmd_web(a):
    need_upstream()
    site = BUILD / "web"
    site.mkdir(parents=True, exist_ok=True)
    for old in site.iterdir():                      # empty it, keep the folder (a dev server may sit in it)
        shutil.rmtree(old) if old.is_dir() else old.unlink()
    shutil.copytree(ROOT / "web", site, dirs_exist_ok=True)
    (site / "vendor").mkdir()
    for f in ("fm1ota.js", "fm1pkg.js"):            # Felucca's proven update protocol and package reader
        shutil.copy(UP / "web" / f, site / "vendor" / f)
    shutil.copy(ROOT / "assets" / "bluerose.png", site / "rose.png")
    for doc in ("MANUAL.md", "BUILDING.md", "README.md", "LICENSE"):
        if (ROOT / doc).exists():
            shutil.copy(ROOT / doc, site / doc)
    cc = find_cc("WASM_CC", True)
    target = "wasm32-freestanding" if "ziglang" in cc else "wasm32"   # zig's name / clang's (and WASI SDK's)
    run([*cc, "-target", target, "-O2", "-std=c99", "-Wall",
         "-Wno-unused-function", "-nostdlib", "-fvisibility=hidden", "-Wl,--no-entry", "-Wl,--export-dynamic",
         "-Wl,-z,stack-size=131072", "-o", site / "emu" / "emb.wasm", ROOT / "src" / "platform" / "wasm.c"])
    pkg = BUILD / "fw" / "emb.fwsc"
    if getattr(a, "with_fw", False) and pkg.exists():
        fdir = site / "firmware"
        fdir.mkdir()
        shutil.copy(pkg, fdir / "emb.fwsc")
        meta = json.loads((BUILD / "fw" / "emb.json").read_text())
        (fdir / "firmware.json").write_text(json.dumps({**meta, "file": "emb.fwsc"}, indent=2))
    (site / ".nojekyll").write_text("")
    print(f"site     {site}  (serve it: python3 -m http.server -d {site.relative_to(ROOT)})")


# ------------------------------------------------------------------ firmware

def cmd_check(_a):
    """the FM-1 port against Felucca's real headers, with a stock clang (a 32-bit RISC-V stand-in for pi32v2):
    every warning is an error, and no variable-width 64-bit shift may remain (pi32v2 links no runtime library)"""
    need_upstream()
    cc = find_cc("WASM_CC", True)
    out = BUILD / "check"
    out.mkdir(parents=True, exist_ok=True)
    target = "riscv32-freestanding" if "ziglang" in cc else "riscv32-unknown-elf"
    run([*cc, "-target", target, "-S", "-emit-llvm", "-o", out / "fm1.ll", "-std=gnu99", "-Os", "-fno-builtin",
         "-Wall", "-Werror", "-Wno-unused-function", "-Wno-unused-command-line-argument",
         "-I", FW / "hal", "-I", FW / "src", ROOT / "src" / "platform" / "fm1.c"])
    ir = (out / "fm1.ll").read_text()
    shifts = re.findall(r"(?:shl|lshr|ashr) i64 [^,]+, %", ir)
    if shifts:
        raise SystemExit(f"check: {len(shifts)} variable 64-bit shift(s): a libcall the pi32v2 link cannot resolve")
    need = {"fm1_cstart", "fm1_alnk0_irq", "fm1_timer5_irq", "fm1_fault_c"}   # what crt0.S / fm1_isr.S / fm1_vec.S call
    missing = [f for f in need if not re.search(r"^define .*@" + f + r"\(", ir, re.M)]
    if missing:
        raise SystemExit(f"check: not defined: {missing}")
    print("check    the FM-1 port compiles clean against Felucca's HAL; entry points and ISRs present")


def toolchain():
    tc = os.environ.get("JIELI_TOOLCHAIN", str(Path.home() / ".jieli" / "toolchain"))
    if not (Path(tc) / "pi32v2" / "bin" / "clang").exists():
        raise SystemExit("JIELI_TOOLCHAIN must point at the JieLi Linux toolchain (pi32v2/ and common/): "
                         "sh upstream/felucca/tools/get_toolchain.sh")
    return Path(tc).resolve()


def use_docker():
    native = platform.system() == "Linux" and platform.machine() in ("x86_64", "AMD64")
    return os.environ.get("JIELI_DOCKER", "0" if native else "1") == "1"


def tc(tool, *args):
    """a toolchain binary, cwd ROOT, paths relative to ROOT (so the container sees them under /work)"""
    rel = [str(Path(a).resolve().relative_to(ROOT)).replace("\\", "/") if isinstance(a, Path) else a for a in args]
    if tool == "cc":
        tool, rel = "pi32v2/bin/clang", ["-target", "pi32v2", *rel]
    if use_docker():
        cmd = ["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{ROOT}:/work",
               "-v", f"{toolchain()}:/opt/jieli:ro", "-w", "/work", DOCKER_IMAGE, f"/opt/jieli/{tool}", *rel]
    else:
        cmd = [str(toolchain() / tool), *rel]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"build: {tool} failed")
    if r.stderr.strip():
        sys.stderr.write(r.stderr)
    return r.stdout


def felucca_build():
    """Felucca's tools/build.py, for its pure helpers (the loader's container, the SDK digests)"""
    sys.path.insert(0, str(UP / "tools"))
    import importlib.util
    spec = importlib.util.spec_from_file_location("felucca_build", UP / "tools" / "build.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def build_loader(fb, out):
    """Felucca's update loader, unchanged: the device runs it to write a package (ota-FM-1_900)"""
    src = FW / "loader"
    flags = [*CFLAGS_FW, "-I" + str((FW / "hal").relative_to(ROOT)).replace("\\", "/"),
             "-I" + str((FW / "src").relative_to(ROOT)).replace("\\", "/")]
    tc("cc", "-c", src / "crt0_ldr.S", "-o", out / "crt0_ldr.o")
    tc("cc", *flags, "-c", src / "loader.c", "-o", out / "loader.o")
    tc("pi32v2/bin/ld", "--gc-sections", "-e", "_start", "-T", src / "loader.ld",
       out / "crt0_ldr.o", out / "loader.o", "-o", out / "loader.elf")
    tc("common/bin/objcopy", "-O", "binary", "-j", ".text", out / "loader.elf", out / "loader.bin")
    image = (out / "loader.bin").read_bytes()
    if not image or len(image) > 0x14000:
        raise SystemExit("loader: image empty or too big")
    return fb.ldr_wrap(image)


def build_app(out):
    flags = [*CFLAGS_FW, "-I" + str((FW / "hal").relative_to(ROOT)).replace("\\", "/"),
             "-I" + str((FW / "src").relative_to(ROOT)).replace("\\", "/"), f'-DFELUCCA_ID="{PRODUCT}"']
    for s, o in ((FW / "crt0.S", "crt0.o"), (FW / "hal" / "fm1_vec.S", "fm1_vec.o"), (FW / "hal" / "fm1_isr.S", "fm1_isr.o")):
        tc("cc", "-c", s, "-o", out / o)
    tc("cc", *flags, "-c", ROOT / "src" / "platform" / "fm1.c", "-o", out / "emb.o")
    elf = out / "emb.elf"
    tc("pi32v2/bin/ld", "-T", FW / "app.ld", out / "crt0.o", out / "fm1_vec.o",   # (Felucca's own app link line)
       out / "fm1_isr.o", out / "emb.o", "-o", elf)
    for sect in ("text", "data", "ram_text"):
        (out / f"{sect}.bin").unlink(missing_ok=True)
        tc("common/bin/objcopy", "-O", "binary", "-j", "." + sect, elf, out / f"{sect}.bin")
    syms = tc("common/bin/objdump", "-t", elf)

    def symv(name):
        return int(re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M).group(1), 16)
    app_xip = 0x02000120                            # app.bin offset 0 in the XIP map: the SPL jumps here
    img = bytearray((out / "text.bin").read_bytes())
    for sect, lname in (("ram_text", "_rt_load"), ("data", "_data_load")):   # after .text, at their load addresses
        blob = (out / f"{sect}.bin").read_bytes() if (out / f"{sect}.bin").exists() else b""
        if blob:
            load = symv(lname)
            if load % 4 or load - app_xip < len(img):
                raise SystemExit(f"{lname} {load:#x}: misplaced")
            img += b"\xff" * (load - app_xip - len(img)) + blob
    img += b"\xff" * (-len(img) % 4)
    return bytes(img)


def cmd_fw(a):
    need_upstream()
    out = BUILD / "fw"
    out.mkdir(parents=True, exist_ok=True)
    fb = felucca_build()
    import fm1pkg_make
    fm1pkg_make.SDK = os.environ.get("AC79_SDK", str(Path.home() / "fw-AC79_AIoT_SDK"))
    for rel, sha in fb.SDK_SHA256.items():
        if hashlib.sha256(fm1pkg_make.sdk_file(rel)).hexdigest() != sha:
            print(f"warning: SDK {rel} differs from AC79NN_SDK_V1.2.1 (the tested one)")
    ota = build_loader(fb, out)
    img = build_app(out)
    if len(img) > fm1pkg_make.APP_SLOT:
        raise SystemExit(f"app: {len(img)} B, the slot holds {fm1pkg_make.APP_SLOT}")
    (out / "emb.bin").write_bytes(img)
    pkg = fm1pkg_make.ufw(fm1pkg_make.flash_image(img, fm1pkg_make.KEY), ota, PRODUCT)
    (out / "emb.fwsc").write_bytes(pkg)
    meta = {"product": PRODUCT, "version": a.version, "sha256": hashlib.sha256(pkg).hexdigest(), "bytes": len(pkg)}
    (out / "emb.json").write_text(json.dumps(meta, indent=2))
    print(f"app      {out / 'emb.bin'}  {len(img)} B of {fm1pkg_make.APP_SLOT}")
    print(f"loader   {len(ota)} B (Felucca's update loader)")
    print(f"package  {out / 'emb.fwsc'}  {len(pkg)} B, identity {PRODUCT}, sha256 {meta['sha256'][:16]}..")


def cmd_all(a):
    cmd_fw(a)
    a.with_fw = True
    cmd_web(a)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("test").set_defaults(fn=cmd_test)
    sub.add_parser("check").set_defaults(fn=cmd_check)
    w = sub.add_parser("web")
    w.add_argument("--with-fw", action="store_true", help="put build/fw/emb.fwsc on the site for the installer")
    w.set_defaults(fn=cmd_web)
    for name, fn in (("fw", cmd_fw), ("all", cmd_all)):
        p = sub.add_parser(name)
        p.add_argument("--version", default="1.0")
        p.set_defaults(fn=fn)
    p = sub.add_parser("wav")
    p.add_argument("slot", type=lambda s: int(s) - 1, help="1..16")
    p.add_argument("seconds", type=int)
    p.set_defaults(fn=cmd_wav)
    sub.add_parser("shots").set_defaults(fn=cmd_shots)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
