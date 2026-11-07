#!/usr/bin/env python3
"""Build the remote's theme packages from remote/themes/<name>/.

    python3 tools/remote_render/build_theme.py [NAME ...] [-o OUT] [--tar DIR]

Each theme directory holds theme.json (copied as it is) and sources.json, which names where
the theme's fonts and icons come from (sources in icon_sets.json, at pinned versions). The
package OUT/<name>/ is what the remote reads from /data/themes/<name>/:

  theme.json                  palette, fonts, layout, wallpaper
  fonts/<role>.faa            fonts converted by the host renderer ("font")
  icons/<keyword>_small.png   activity icons, 40 px (list); <keyword>.png 70 px (activity screen)
  stock/<path>                replacements for stock assets (status bar, menu and settings glyphs)
  LICENSES.md                 licences of the fonts and icon sets used

--tar DIR also writes each package as DIR/<name>.tar and the list of themes as DIR/index.json,
for the web UI's screen preview (tools/remote_render/web/remote_preview.js, loadTheme).

Downloads are cached under build/tmp/remote-render/svg; nothing downloaded is committed.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import tarfile
import urllib.parse
import urllib.request
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
THEMES = os.path.join(REPO, "remote", "themes")

LIST_ICON, HERO_ICON = 40, 70  # remote_ui_activities.c, HERO_ICON in remote_ui_activity.c


def fetch(url, cache):
    """Download url to the cache path once; return (path, None) or (None, error)"""
    if not os.path.exists(cache):
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        try:
            data = urllib.request.urlopen(urllib.parse.quote(url, safe=":/@"), timeout=30).read()
        except Exception as e:  # noqa: BLE001 - report and continue with the other icons
            return None, str(e)
        with open(cache, "wb") as f:
            f.write(data)
    return cache, None


def solid_png(path, w, h, colour):
    """A w x h PNG of one colour (#RRGGBB)"""
    rgb = bytes.fromhex(colour.lstrip("#"))
    raw = b"".join(b"\x00" + rgb * w for _ in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


class Builder:
    def __init__(self, cfg, work, renderer):
        self.cfg, self.work, self.renderer = cfg, work, renderer
        self.used = set()

    def download(self, src_id, name):
        src = self.cfg["sources"][src_id]
        if "path" in src:  # drawn for the project, kept in the repository
            self.used.add(src_id)
            return os.path.join(REPO, src["path"].format(name=name)), src.get("pad", 0.0)
        url = src["url"].format(name=name)
        path, err = fetch(url, os.path.join(self.work, "svg", src_id, name.replace("/", "_")))
        if not path:
            raise SystemExit(f"[-] {url}: {err}")
        self.used.add(src_id)
        return path, src.get("pad", 0.0)

    def icon(self, src_id, name, size, colour, out):
        svg, pad = self.download(src_id, name)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        cmd = [self.renderer, "rasterize", "--svg", svg, "--size", str(size), "--pad", str(pad), "-o", out]
        if colour:
            cmd += ["--color", colour]
        subprocess.run(cmd, check=True)

    def licences(self, out):
        lines = ["# Licences", "", "This theme package contains files made from these works.", ""]
        by_url = {}
        for src_id in sorted(self.used):
            src = self.cfg["sources"][src_id]
            by_url.setdefault(src.get("licence_url") or src["licence_file"], (src.get("title", src_id), src["licence"]))
        for url, (title, licence) in by_url.items():
            if "//" not in url:
                text, err = os.path.join(REPO, url), None
            else:
                text, err = fetch(url, os.path.join(self.work, "svg", "licences", url.split("//", 1)[1].replace("/", "_")))
            if not text:
                raise SystemExit(f"[-] {url}: {err}")
            lines += [f"## {title} ({licence})", "", open(text, encoding="utf-8").read().strip(), ""]
        with open(out, "w") as f:
            f.write("\n".join(lines))


def build(name, cfg, out_root, builder, themes=THEMES):
    src_dir = os.path.join(themes, name)
    sources = json.load(open(os.path.join(src_dir, "sources.json")))
    out = os.path.join(out_root, name)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    shutil.copy(os.path.join(src_dir, "theme.json"), out)
    builder.used = set()

    for role, f in sources.get("fonts", {}).items():
        ttf, _ = builder.download(f["source"], role)
        os.makedirs(os.path.join(out, "fonts"), exist_ok=True)
        subprocess.run([builder.renderer, "font", "--ttf", ttf, "--px", str(f["px"]), "--height", str(f["height"]),
                        "--baseline", str(f["baseline"]), "-o", os.path.join(out, "fonts", f"{role}.faa")], check=True)

    kw = sources.get("keywords")
    if kw:
        for keyword in cfg["keywords"]:
            own = cfg["generic"][kw["map"]].get(keyword)
            if own:
                src_id, icon = kw["set"], own
            else:  # a theme can draw a brand keyword from another source ("brands" in sources.json)
                src_id, icon = kw.get("brands", {}).get(keyword) or cfg["brands"][keyword]
            # "tint" colours each keyword with its own colour from icon_sets.json
            colour = cfg["tint"][keyword] if kw.get("color") == "tint" else kw.get("color")
            for size, suffix in ((LIST_ICON, "_small"), (HERO_ICON, "")):
                builder.icon(src_id, icon, size, colour, os.path.join(out, "icons", f"{keyword}{suffix}.png"))

    glyphs = sources.get("glyphs", {})
    for path, (src_id, icon, size) in glyphs.get("icons", {}).items():
        builder.icon(src_id, icon, size, glyphs.get("color"), os.path.join(out, "stock", path))
    for path, (w, h, colour) in sources.get("solid", {}).items():
        os.makedirs(os.path.dirname(os.path.join(out, "stock", path)), exist_ok=True)
        solid_png(os.path.join(out, "stock", path), w, h, colour)

    builder.licences(os.path.join(out, "LICENSES.md"))
    print(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="themes to build (default: every theme in remote/themes)")
    ap.add_argument("--work", default=os.path.join(REPO, "build", "tmp", "remote-render"), help="build directory")
    ap.add_argument("-o", "--out", default=os.path.join(REPO, "build", "tmp", "remote-themes"))
    ap.add_argument("--tar", help="also write <name>.tar and index.json here")
    ap.add_argument("--themes", default=THEMES, help="directory of theme sources (default: remote/themes)")
    args = ap.parse_args()

    cfg = json.load(open(os.path.join(HERE, "icon_sets.json")))
    renderer = os.path.join(args.work, "remote_render")
    if subprocess.run([os.path.join(HERE, "build.sh"), renderer], stdout=subprocess.DEVNULL).returncode != 0:
        raise SystemExit("[-] Cannot build the host renderer (needs a C compiler)")
    builder = Builder(cfg, args.work, renderer)
    names = args.names or sorted(d for d in os.listdir(args.themes) if os.path.isfile(os.path.join(args.themes, d, "sources.json")))
    for name in names:
        build(name, cfg, args.out, builder, args.themes)
    if args.tar:
        os.makedirs(args.tar, exist_ok=True)
        index = [{"id": "classic", "name": "Classic"}]
        for name in names:
            with tarfile.open(os.path.join(args.tar, f"{name}.tar"), "w", format=tarfile.USTAR_FORMAT) as t:
                for entry in sorted(os.listdir(os.path.join(args.out, name))):
                    t.add(os.path.join(args.out, name, entry), arcname=entry)
            index.append({"id": name, "name": json.load(open(os.path.join(args.out, name, "theme.json"))).get("name", name)})
        with open(os.path.join(args.tar, "index.json"), "w") as f:
            json.dump(index, f)


if __name__ == "__main__":
    main()
