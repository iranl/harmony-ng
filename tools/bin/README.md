# Prebuilt binaries kept in the repository

These files are committed as binaries. No build script in the repository produces them, and
the release ships them as they are, so they cannot be rebuilt and compared today. This file
records what is known about each one, so a change to any of them shows up in the diff.

| File | What it is | SHA-256 | Notes |
|---|---|---|---|
| `tools/bin/dropbearmulti_mips` | Hub SSH server (multi-call: dropbear, dropbearkey, scp) | `65c53c9534be562c0335138489ddd5712782898f1c93e85608f393dc4f54604f` | MIPS32 big-endian, static, 303,628 bytes. Reports `SSH-2.0-dropbear_2026.94`. Shipped by `build/compile_all.sh` and the release as `dropbearmulti`. Built by `build/build_minimal_dropbear.sh` (dropbear 2026.94 from a tarball it expects in `build/tmp/dropbear/`, Bootlin `mips32--uclibc--stable-2017.05` toolchain; the script holds the options: Ed25519, curve25519 and chacha20-poly1305 only, no password auth, no TCP or stream forwarding, no client: `dbclient` is not in it) and copied here by hand. |
| `remote/bin/dropbearmulti_musl` | Remote SSH server | `d8b1ce9a8df2003a85915641bffe0f6a329536adc7ccab860744c2f9b361d868` | ARM EABI5, static-pie (musl). Reports `SSH-2.0-dropbear_2024.85`. No build script. |
| `payload/modules/*.ko` (13 files) | USB Ethernet drivers for the Hub's 2.6.31 kernel | see below | `vermagic=2.6.31-g89d565c mod_unload modversions MIPS32_R2 32BIT`. `build/build_usb_ethernet.sh` builds them from a copy of `linux-2.6.31.14` configured like the stock kernel (no SMP, hotplug, suspend or LBDAF), with the CRCs of the stock image and stock `usbcore.ko` and the AX88772B ID patch (`build/patches/usb_net/asix-ax88772b.patch`). |
| `remote/b64dec_tiny` | Small base64 decoder used while rooting the remote | `0a21a5d8aa3760b6d9e149bead49e6f66acdd236935381fa36029374bda08c94` | Its source is `remote/b64dec_tiny.c`; no build script compiles it. |
| `tools/unbrick_recovery/recv_static` | MIPS program kept with the Hub recovery tools, named after the `/tmp/recv` file receiver | `1efbdadfed6ddc2408086d3cbbc069f46f47a637779ff32e5681c2825c2dcf0b` | MIPS32 big-endian, static, 99,956 bytes. Its compiler string is `GCC: (Buildroot 2017.05) 5.4.0`. No source and no build script in the repository, and no guide, tool or script refers to it. Not in the release bundles. |
| `tools/unbrick_recovery/upload_recv.sh` | Script pasted into the Hub's serial shell: it writes a small TCP file receiver to `/tmp/recv` | `5328eb86a9f9c561b525033440e37608322a4cd9aa2b7fa3410f679b8cbf4b27` | The hash is that of the script. The receiver is inside it as 68 `echo -ne` lines: MIPS32 big-endian, linked against the Hub's uClibc, 4,332 bytes, SHA-256 `1f9a076f4c488679e3aaa3121d31ab39de62273176ba9af458a7a5a6decf06bc` once written. No source in the repository. Used by `docs/OFFLINE_UNBRICK_AND_ROOT_GUIDE.md`. Not in the release bundles. |

Kernel modules (SHA-256):

```
3ebab378f105eef00aba5938dc068f8dfca0259a015ce18f91a370e0df06fb70  asix.ko
17f3a0e0c3a518764bd9e244d13440979d5cb189972e549ed785b7e4e5843224  cdc_ether.ko
843f2f3948018fd515b215236eef28a89f0ef1e6dd96585fe28f6e5445ddc09d  cdc_subset.ko
93065d488a00dcd311c35493581f9a9d798516587797f800641625cc949ab730  dm9601.ko
9701b1edd76f8b1651a0ac9a287e9194938430844bbb26e66ab46f041275620f  mcs7830.ko
2e60f0d99ffdfdd55a32a22faa15decb666985cc331851a9d24347cb9c5a7501  net1080.ko
62df3cb0df7d5434c84e1003e6ec0c872db23932cf449ec48ed9d3003c8adec2  pegasus.ko
e28d0883d535ca4d6b04dc8b84b14705a210b0dd860ae6896354fe16c8e15676  r8152.ko
3b3cbf4d47519477c5e180cec3762daaacd412dc289a9223b57a6a10304ae4dc  rndis_host.ko
d402d547d694ef36d5353e602fa2e5cb2bb2e2d36a67bd688f95b32a995d621e  rtl8150.ko
3672be02773caea21c97a5facefe733994d247898248dc5e41e875c1d44a320a  smsc95xx.ko
735578312f26e3059d9c332155eba065d5d125eeb2b9c14ee801794cd1a4f301  usbnet.ko
0413f3a4d46c720a7e43a0cb0ce23a0885bb237d5aab138e1ce0c6be0279687b  zaurus.ko
```

`tests/test_prebuilt_hashes.sh` checks the files against this list. Whoever replaces one of
them updates the list in the same commit, with the source version, the options and the
toolchain used, so the change is visible in the diff.
