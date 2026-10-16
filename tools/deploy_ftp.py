#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Upload build/web/ (python3 tools/build.py web --with-fw) to the website over FTP, replacing what is there.

  python3 tools/deploy_ftp.py [--host fm1.miketsak.gr] [--user fm1] [--dir REMOTE_DIR] [--plain]

The password comes from the FM1_FTP_PASS environment variable, or is asked for. It is never stored.
TLS (explicit FTPS) is used; --plain allows unencrypted FTP, which sends the password in the clear.
Without --dir the site goes to the login folder, or to httpdocs/ when that is where the site lives (Plesk).
"""
import argparse
import ftplib
import getpass
import json
import os
import sys
from pathlib import Path

SITE = Path(__file__).resolve().parents[1] / "build" / "web"


def load_dotenv(path):
    if not path.is_file():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.strip()
        v = v.strip().strip("'\"")
        if k:
            os.environ.setdefault(k, v)


def connect(a, password):
    if a.plain:
        ftp = ftplib.FTP(a.host, timeout=30)
        ftp.login(a.user, password)
    else:
        ftp = ftplib.FTP_TLS(a.host, timeout=30)
        ftp.login(a.user, password)
        ftp.prot_p()                                # the files go encrypted too
    return ftp


def site_dir(ftp, wanted):
    if wanted:
        return wanted
    names = set(ftp.nlst())
    if "index.html" not in names and "httpdocs" in names:
        return "httpdocs"
    return "."


def mkdirs(ftp, path):
    try:
        ftp.mkd(path)
    except ftplib.error_perm:
        pass                                        # (already there)


def main():
    load_dotenv(Path(__file__).resolve().parents[1] / ".env")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("FM1_FTP_HOST", "fm1.miketsak.gr"))
    ap.add_argument("--user", default=os.environ.get("FM1_FTP_USER", "fm1"))
    ap.add_argument("--dir", help="remote folder of the site (default: found automatically)")
    ap.add_argument("--plain", action="store_true", help="unencrypted FTP (only if the server has no TLS)")
    a = ap.parse_args()
    if not (SITE / "index.html").exists():
        sys.exit("build/web is empty: run  python3 tools/build.py web --with-fw  first")
    fw = SITE / "firmware" / "firmware.json"
    print(f"site     {SITE}  " + (f"(firmware v{json.loads(fw.read_text())['version']})" if fw.exists() else
                                   "(no firmware: the installer will only offer a file of your own)"))
    password = os.environ.get("FM1_FTP_PASS") or getpass.getpass(f"FTP password for {a.user}@{a.host}: ")
    try:
        ftp = connect(a, password)
    except ftplib.all_errors as e:
        sys.exit(f"could not log in: {e}" + ("" if a.plain else "  (no TLS on the server? try --plain)"))
    with ftp:
        root = site_dir(ftp, a.dir)
        ftp.cwd(root)
        print(f"remote   {a.host}:{ftp.pwd()}")
        files = sorted(p for p in SITE.rglob("*") if p.is_file())
        for d in sorted({p.parent.relative_to(SITE) for p in files}):
            if str(d) != ".":
                mkdirs(ftp, d.as_posix())
        for i, p in enumerate(files, 1):
            rel = p.relative_to(SITE).as_posix()
            with open(p, "rb") as f:
                ftp.storbinary(f"STOR {rel}", f)
            print(f"  [{i:2}/{len(files)}] {rel}")
    print(f"done     https://{a.host}/  (hard-refresh: Ctrl+F5)")


if __name__ == "__main__":
    main()
