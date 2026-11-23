#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# OrganVave's site with the installer: one self-contained page (site_landing.html: the installer's logic and its texts, the
# FM-1's package and the SMK-37's three embedded). The installer's JS (fm1pkg.js, fm1ota.js) is the FMini-1 site's
# (GPL-3.0-only). Each package goes to its own model: the device found picks it (as on FMini-1).
#   python3 web/make_site.py build/felucca.fwsc OUTDIR VERSION [PRO.fwsc ELITE.fwsc MKE.fwsc]
# writes OUTDIR/index.html and the packages beside it (organvave-fm1-vVERSION.fwsc, smk/organvave-smk37[-elite|-mke]-vVERSION.fwsc).
# Without the three SMK-37 packages the page carries the FM-1's only (a local check; the Installer workflow passes all four).
# VERSION must be ORGANVAVE_VERSION in firmware/src/core.h. Serve it over https or http://localhost: Web MIDI with SysEx
# needs a secure context (Chrome or Edge).
import base64
import json
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BLOCKS, BLK, KEEP = 20, 0x30, 0x2F
SMK = (("SMK-37 Pro", ""), ("SMK-37 Elite", "-elite"), ("MKE-P37", "-mke"))


def strip_module(src):
    src = re.sub(r"^export\s+", "", src, flags=re.M)
    return re.sub(r"^import .*?;\n", "", src, flags=re.M)


def product_of(raw):
    """the package identity: one marker byte after each of the first 20 blocks (fm1pkg.js productOf)"""
    return "".join(chr((m - i - 1) & 0xFF) for i in range(BLOCKS) if (m := raw[i * BLK + KEEP]) != 0x7D)


def package(path, want, label, version, file):
    raw = Path(path).read_bytes()
    product = product_of(raw)
    if not re.fullmatch(re.escape(want) + r"_8\d\d", product) or b"FELUCCA-LOADER-1" not in raw:
        raise SystemExit(f"{path}: not the {want} package of this firmware (identity {product!r})")
    return raw, {"product": product, "label": label, "file": file, "version": version,
                 "pkg": "data:application/octet-stream;base64," + base64.b64encode(raw).decode()}


def main(pkg, outdir, version, *smk):
    core = (ROOT / "firmware" / "src" / "core.h").read_text(encoding="utf-8")
    if f'#define ORGANVAVE_VERSION "v{version}"' not in core:
        raise SystemExit(f"VERSION {version} is not ORGANVAVE_VERSION in firmware/src/core.h")
    if smk and len(smk) != 3:
        raise SystemExit("the SMK-37 packages: PRO ELITE MKE (three files) or none")
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    fm_file = f"organvave-fm1-v{version}.fwsc"
    entries, files = [], []
    raw, e = package(pkg, "FM-1", "FM-1", version, fm_file)
    entries.append(e)
    files.append((raw, out / fm_file))
    for (model, suffix), path in zip(SMK, smk):
        file = f"smk/organvave-smk37{suffix}-v{version}.fwsc"
        raw, e = package(path, model, model, version, file)
        entries.append(e)
        files.append((raw, out / file))
    html = (HERE / "site_landing.html").read_text(encoding="utf-8")
    lib = "\n".join(strip_module((HERE / f).read_text(encoding="utf-8")) for f in ("fm1pkg.js", "fm1ota.js"))
    for mark in ("/*LIB*/", "/*META*/{}"):
        if html.count(mark) != 1:
            raise SystemExit(f"site_landing.html must contain {mark} once")
    meta = json.dumps({"version": version, "smkVersion": version, "pkgs": entries})
    html = html.replace("/*LIB*/", lib).replace("/*META*/{}", meta).replace("/*VERSION*/", version)
    (out / "index.html").write_text(html, encoding="utf-8")       # the home page: the installer and the packages embedded
    for raw, dest in files:                                       # the downloads, beside it
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(raw)
    print(f"{out}/index.html ({len(html)} B): " + ", ".join(f"{x['product']} ({x['file']})" for x in entries))


if __name__ == "__main__":
    if len(sys.argv) not in (4, 7):
        sys.exit(__doc__)
    main(*sys.argv[1:])
