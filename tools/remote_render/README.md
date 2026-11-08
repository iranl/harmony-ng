# Remote screen renderer (host)

Draws the Harmony remote's screens on a computer with the firmware's own drawing code (`payload/source/remote`), and writes PNGs.

## Build

```sh
tools/remote_render/build.sh            # writes build/tmp/remote-render/remote_render
```

It needs a C compiler and nothing else. The remote's hardware calls that the drawing code can reach (RF, power, display, clock) are implemented as no-ops in `host_platform.c`; `render_state.c` sets up the state for a render.

```sh
tools/remote_render/build.sh --wasm                            # needs Emscripten; writes build/tmp/remote-render/web
tools/remote_render/build.sh --wasm android-app/public/assets  # the legacy web UI's copy (CI and release builds do this)
tools/remote_render/build.sh --wasm frontend/public/assets     # the new web UI's copy (build/deploy_hub.py does this when emcc is installed)
```

The WebAssembly build is the same code with `remote_render_wasm.c` as its entry point: about 86 KB of WebAssembly and 18 KB of JavaScript, gzipped. It writes `remote_render.js`, `remote_render.wasm` and `web/remote_preview.js`, which loads files into the module, writes the config the way the remote stores the Hub's push, and draws into a canvas. Both web UIs import `remote_preview.js` from their assets on the remote page: the legacy UI (`android-app/src/app/app_remotes_screen.js`) keeps its HTML mock-up when the files are missing, and the new one (`frontend/src/views/setup/remote.js`) leaves its Remote Screen card hidden. `web/index.html` is a development page, copied only into the default directory. To try it, put an uncompressed tar of a filesystem root next to the page and serve the directory:

```sh
(cd ROOT && tar --format ustar -cf ../build/tmp/remote-render/web/root.tar .)
(cd build/tmp/remote-render/web && python3 -m http.server 8765)    # open http://localhost:8765
```

## Filesystem root

The renderer reads files as the remote does, under `--root`:

| Path under the root | Content |
|---|---|
| `data/remote_rf_config.json` | Activities, devices and buttons, in the format the remote stores the Hub's config push in. |
| `opt/luadraws/appscommon/asset/` | Stock Logitech assets (wallpaper, status bar, Classic icons). The project does not ship them. Copy the tree from a remote: `ssh root@192.168.2.1 '/data/busybox tar -c -C /opt/luadraws/appscommon/asset .' \| tar -x -C ROOT/opt/luadraws/appscommon/asset`. The Hub's `/api/icons/archive` has the same tree when it was built by `tools/harmony_firmware.py extract-icons`. Older Hubs have a flat archive: stock paths then fall back to the file name, and 359 file names have different images in different directories, so some icons come out wrong. |
| `data/themes/<set>/<keyword>.png` | Activity icons for an icon set, one per keyword (`--icons`, `compare`) |
| `data/themes/<name>/` | Theme packages (`--theme`), as `build_theme.py` writes them |

## Commands

```sh
R=build/tmp/remote-render/remote_render
$R render --root ROOT --view activities --active 10002 -o activities.png
$R render --root ROOT --view commands --active 10001 -o commands.png
$R render --root ROOT --view sync --sync 37/120 -o sync.png
$R sheet --root ROOT --icons /data/themes/phosphor-regular --title "Phosphor" -o keywords.png
$R compare --root ROOT --set "Classic=classic" --set "Phosphor=/data/themes/phosphor-regular" -o compare.png
$R render --root ROOT --theme dark --view settings -o settings-dark.png
$R rasterize --svg icon.svg --size 40 --color '#F5F5FA' -o icon.png
$R font --ttf Inter-Medium.ttf --px 12 --height 15 --baseline 12 -o main.faa
```

`font` converts a TTF or OTF to the `.faa` font format themes use: `--px` pixels per em, in lines `--height` pixels tall with the baseline `--baseline` pixels below the top. The built-in fonts are 15/12 (main) and 19/15 (title).

`render` options: `--theme NAME`, `--layout list|grid` (the home layout; the theme's when unset), `--view` (`activities`, `menu` (the status bar menu open over the activities), `hint` (the one-time key hint), `link` (the RF link panel), `charging` (the page shown after docking), `commands`, `devices`, `device` (the device at `--select`), `settings`, `pairing`, `not-paired`, `sync`), `--select N`, `--active ID`, `--icons DIR`, `--time HH:MM`, `--battery PCT`, `--usb`, `--charging`, `--no-hub`, `--link-loss PCT` (share of link reports and Hub replies lost), `--hub-silent` (link up, Hub not answering), `--scale N`.

## Themes

```sh
python3 tools/remote_render/build_theme.py                 # every theme in remote/themes, into build/tmp/remote-themes
python3 tools/remote_render/build_theme.py dark --tar frontend/public/assets/themes   # also the web UI's copies
```

Builds the theme packages the remote reads from `/data/themes/<name>/` from `remote/themes/<name>/theme.json` and `sources.json`. The sources name icons and fonts in `icon_sets.json`; the script downloads them at pinned versions (cached under `build/tmp/remote-render/svg`), rasterises the icons, converts the fonts and adds their licences. `build/compile_all.sh remote` builds the themes into the remote package and OTA package; `--tar` writes the copies the web UI's screen preview loads.

The icon sets and fonts the themes use:

| Source | Licence |
|---|---|
| [Phosphor](https://github.com/phosphor-icons/core) | MIT |
| [Tabler Icons](https://github.com/tabler/tabler-icons) | MIT |
| [Simple Icons](https://github.com/simple-icons/simple-icons) | CC0 1.0; logos remain trademarks of their owners |
| [Fluent Emoji](https://github.com/microsoft/fluentui-emoji) | MIT |
| [Inter](https://github.com/rsms/inter) (fonts) | SIL Open Font License 1.1 |

## Vendored files

| File | Origin | Licence |
|---|---|---|
| `nanosvg.h`, `nanosvgrast.h` | [memononen/nanosvg](https://github.com/memononen/nanosvg) `239e102` | zlib |
| `stb_image_write.h` | [nothings/stb](https://github.com/nothings/stb) `003501f` (v1.16) | Public domain / MIT |
