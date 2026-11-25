# Prebuilt binaries kept in the repository

These files are committed as binaries. No build script in the repository produces them, and
the release ships them as they are, so they cannot be rebuilt and compared today. This file
records what is known about each one, so a change to any of them shows up in the diff.

| File | What it is | SHA-256 | Notes |
|---|---|---|---|
| `tools/bin/dropbearmulti_mips` | Hub SSH server (multi-call: dropbear, dropbearkey, scp) | `65c53c9534be562c0335138489ddd5712782898f1c93e85608f393dc4f54604f` | MIPS32 big-endian, static, 303,628 bytes. Reports `SSH-2.0-dropbear_2026.94`. Shipped by `build/compile_all.sh` and the release as `dropbearmulti`. Built by `build/build_minimal_dropbear.sh` (dropbear 2026.94 from a tarball it expects in `build/tmp/dropbear/`, Bootlin `mips32--uclibc--stable-2017.05` toolchain; the script holds the options: Ed25519, curve25519 and chacha20-poly1305 only, no password auth, no TCP or stream forwarding, no client: `dbclient` is not in it) and copied here by hand. |
| `remote/bin/dropbearmulti_musl` | Remote SSH server | `d8b1ce9a8df2003a85915641bffe0f6a329536adc7ccab860744c2f9b361d868` | ARM EABI5, static-pie (musl). Reports `SSH-2.0-dropbear_2024.85`. No build script. |
| `payload/modules/*.ko` (13 files) | USB Ethernet drivers for the Hub's 2.6.31 kernel | see below | `vermagic=2.6.31-g89d565c mod_unload modversions MIPS32_R2 32BIT`. `build/build_usb_ethernet.sh` builds them from kernel.org's `linux-2.6.31.14.tar.xz` (SHA-256 checked) with `build/patches/usb_net/kernel-tree.patch`, the AX88772B ID patch (`asix-ax88772b.patch`) and `kernel.config` (like the stock kernel: no SMP, hotplug, suspend, LBDAF, IPv6 or XFRM, `HZ` 250; `stock_layout.c` stops the build unless `struct sk_buff`, the USB structs, `HZ` and the kmalloc table match the stock kernel), with the CRCs of the stock image and stock `usbcore.ko`. `r8152.ko` is not rebuilt. |
| `remote/b64dec_tiny` | Small base64 decoder used while rooting the remote | `0a21a5d8aa3760b6d9e149bead49e6f66acdd236935381fa36029374bda08c94` | Its source is `remote/b64dec_tiny.c`; no build script compiles it. |
| `tools/unbrick_recovery/recv_static` | MIPS program kept with the Hub recovery tools, named after the `/tmp/recv` file receiver | `1efbdadfed6ddc2408086d3cbbc069f46f47a637779ff32e5681c2825c2dcf0b` | MIPS32 big-endian, static, 99,956 bytes. Its compiler string is `GCC: (Buildroot 2017.05) 5.4.0`. No source and no build script in the repository, and no guide, tool or script refers to it. Not in the release bundles. |
| `tools/unbrick_recovery/upload_recv.sh` | Script pasted into the Hub's serial shell: it writes a small TCP file receiver to `/tmp/recv` | `5328eb86a9f9c561b525033440e37608322a4cd9aa2b7fa3410f679b8cbf4b27` | The hash is that of the script. The receiver is inside it as 68 `echo -ne` lines: MIPS32 big-endian, linked against the Hub's uClibc, 4,332 bytes, SHA-256 `1f9a076f4c488679e3aaa3121d31ab39de62273176ba9af458a7a5a6decf06bc` once written. No source in the repository. Used by `docs/OFFLINE_UNBRICK_AND_ROOT_GUIDE.md`. Not in the release bundles. |

Kernel modules (SHA-256):

```
79dc78da2448369b5cd3da63b885c93f23c6d6fd48d628707fc7a5bc6fe15c21  asix.ko
ea7e40f34c53a17030ba997d0512199296f3c50a49fc2bed7df0f04ba497c216  cdc_ether.ko
843f2f3948018fd515b215236eef28a89f0ef1e6dd96585fe28f6e5445ddc09d  cdc_subset.ko
cc8d70d9db5ed8841eeb8756db6f954dfab45bb91557621e7118428b8e52ceee  dm9601.ko
5c2312b9d4e6d253a068ffe20c9afab5af64cadac5a5eab6e394a0ac0ef38a24  mcs7830.ko
5e0c83889946e9aabe13531060a6694a0e1164b3fbeedbc3a3b4630e84989a00  net1080.ko
658ac8c1b92cee9de55cd71410b6ed573e8d4baf2c9dc4bd29eacdda8ffd77e0  pegasus.ko
e28d0883d535ca4d6b04dc8b84b14705a210b0dd860ae6896354fe16c8e15676  r8152.ko
2600d2fc01dc4cfe27807a62414d76a562e055b93ea27c5969bb31b2ecfcbfca  rndis_host.ko
acad673b08f389ec1ea05ed4ead3e9960423bb68af68adf548d4e299d6d0de50  rtl8150.ko
938bd66bc25e1959a45b8d47ea04680f98ef6a0ed4fec8ecef8dd74d1c28409b  smsc95xx.ko
954a6ba993beb7995d07d8e4716018adf1175a1899b90643d5c03bacbf376fb7  usbnet.ko
533f9c533ec788bcbac08d54df0988a1bb1a72df5b6ddc1df17d2a820988a394  zaurus.ko
```

`tests/test_prebuilt_hashes.sh` checks the files against this list. Whoever replaces one of
them updates the list in the same commit, with the source version, the options and the
toolchain used, so the change is visible in the diff.
