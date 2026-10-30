# Elite screen renderer (host)

Draws the Harmony Elite's screens on a computer with the firmware's own drawing code (`payload/source/elite`), and writes PNGs.

## Build

```sh
tools/elite_render/build.sh            # writes build/tmp/elite-render/elite_render
```

It needs a C compiler and nothing else. The remote's hardware calls that the drawing code can reach (RF, power, display, clock) are implemented as no-ops in `host_platform.c`; `render_state.c` sets up the state for a render.

```sh
tools/elite_render/build.sh --wasm                            # needs Emscripten; writes build/tmp/elite-render/web
tools/elite_render/build.sh --wasm android-app/public/assets  # the web UI's copy (CI and release builds do this)
```

The WebAssembly build is the same code with `elite_render_wasm.c` as its entry point: about 86 KB of WebAssembly and 18 KB of JavaScript, gzipped. It writes `elite_render.js`, `elite_render.wasm` and `web/elite_preview.js`, which loads files into the module and draws into a canvas. The web UI's Elite page imports `elite_preview.js` (`android-app/src/elite-screen.js`) and keeps its HTML mock-up when the files are missing. `web/index.html` is a development page, copied only into the default directory. To try it, put an uncompressed tar of a filesystem root next to the page and serve the directory:

```sh
(cd ROOT && tar --format ustar -cf ../build/tmp/elite-render/web/root.tar .)
(cd build/tmp/elite-render/web && python3 -m http.server 8765)    # open http://localhost:8765
```

## Filesystem root

The renderer reads files as the remote does, under `--root`:

| Path under the root | Content |
|---|---|
| `data/elite_rf_config.json` | Activities, devices and buttons, in the format the remote stores the Hub's config push in. |
| `opt/luadraws/appscommon/asset/` | Stock Logitech assets (wallpaper, status bar, Classic icons). The project does not ship them. Copy the tree from a remote: `ssh root@192.168.2.1 '/data/busybox tar -c -C /opt/luadraws/appscommon/asset .' \| tar -x -C ROOT/opt/luadraws/appscommon/asset`. The Hub's `/api/icons/archive` has the same tree when it was built by `tools/harmony_firmware.py extract-icons`. Older Hubs have a flat archive: stock paths then fall back to the file name, and 359 file names have different images in different directories, so some icons come out wrong. |
| `data/themes/<set>/<keyword>.png` | Activity icons for an icon set, one per keyword |

## Commands

```sh
R=build/tmp/elite-render/elite_render
$R render --root ROOT --view activities --active 10002 -o activities.png
$R render --root ROOT --view commands --active 10001 -o commands.png
$R render --root ROOT --view sync --sync 37/120 -o sync.png
$R sheet --root ROOT --icons /data/themes/phosphor-regular --title "Phosphor" -o keywords.png
$R compare --root ROOT --set "Classic=classic" --set "Phosphor=/data/themes/phosphor-regular" -o compare.png
$R rasterize --svg icon.svg --size 40 --color '#F5F5FA' -o icon.png
```

`render` options: `--view` (`activities`, `commands`, `devices`, `device` (the device at `--select`), `settings`, `pairing`, `not-paired`, `sync`), `--select N`, `--active ID`, `--icons DIR`, `--time HH:MM`, `--battery PCT`, `--usb`, `--charging`, `--no-hub`, `--scale N`.

## Vendored files

| File | Origin | Licence |
|---|---|---|
| `nanosvg.h`, `nanosvgrast.h` | [memononen/nanosvg](https://github.com/memononen/nanosvg) `239e102` | zlib |
| `stb_image_write.h` | [nothings/stb](https://github.com/nothings/stb) `003501f` (v1.16) | Public domain / MIT |
