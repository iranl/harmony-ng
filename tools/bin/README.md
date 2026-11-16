# Prebuilt binaries kept in the repository

These files are committed as binaries. No build script in the repository produces them, and
the release ships them as they are, so they cannot be rebuilt and compared today. This file
records what is known about each one, so a change to any of them shows up in the diff.

| File | What it is | SHA-256 | Notes |
|---|---|---|---|
| `tools/bin/dropbearmulti_mips` | Hub SSH server (multi-call: dropbear, dropbearkey, scp) | `e2ea632aed8b31dc5ea56b9673cbd983ec83260a97d33f891a0cebf51d5c6c8d` | MIPS32 big-endian, static. Reports `SSH-2.0-dropbear_2025.89`. Shipped by `build/compile_all.sh` and the release as `dropbearmulti`. `build/build_minimal_dropbear.sh` builds dropbear 2024.86 from a tarball it expects in `build/tmp/dropbear/` and copies the result nowhere: it is not the source of this file. |
| `remote/bin/dropbearmulti_musl` | Remote SSH server | `d8b1ce9a8df2003a85915641bffe0f6a329536adc7ccab860744c2f9b361d868` | ARM EABI5, static-pie (musl). Reports `SSH-2.0-dropbear_2024.85`. No build script. |
| `payload/modules/*.ko` (13 files) | USB Ethernet drivers for the Hub's 2.6.31 kernel | see below | `vermagic=2.6.31-g89d565c mod_unload modversions MIPS32_R2 32BIT`. `build/build_usb_ethernet.sh` builds them from a kernel tree in `build/tmp/linux-2.6.31.14`; that tree's origin and `.config` are not recorded. |
| `remote/b64dec_tiny` | Small base64 decoder used while rooting the remote | `0a21a5d8aa3760b6d9e149bead49e6f66acdd236935381fa36029374bda08c94` | Its source is `remote/b64dec_tiny.c`; no build script compiles it. |
| `tools/unbrick_recovery/recv_static` | MIPS program kept with the Hub recovery tools, named after the `/tmp/recv` file receiver | `1efbdadfed6ddc2408086d3cbbc069f46f47a637779ff32e5681c2825c2dcf0b` | MIPS32 big-endian, static, 99,956 bytes. Its compiler string is `GCC: (Buildroot 2017.05) 5.4.0`. No source and no build script in the repository, and no guide, tool or script refers to it. Not in the release bundles. |
| `tools/unbrick_recovery/upload_recv.sh` | Script pasted into the Hub's serial shell: it writes a small TCP file receiver to `/tmp/recv` | `5328eb86a9f9c561b525033440e37608322a4cd9aa2b7fa3410f679b8cbf4b27` | The hash is that of the script. The receiver is inside it as 68 `echo -ne` lines: MIPS32 big-endian, linked against the Hub's uClibc, 4,332 bytes, SHA-256 `1f9a076f4c488679e3aaa3121d31ab39de62273176ba9af458a7a5a6decf06bc` once written. No source in the repository. Used by `docs/OFFLINE_UNBRICK_AND_ROOT_GUIDE.md`. Not in the release bundles. |

Kernel modules (SHA-256):

```
2882bbcb33b4cb5fd0536de4767b9236e5f16276704f0cb3e265ce51383c8fd6  asix.ko
0aa31de8d8a45c15fde0a2ad5e725dbddd47084a7e07475fa88dba9443932443  cdc_ether.ko
d1a8a7229def58124d1fcad8a18b500be953495a172eae6e91b68baa7c2d51a7  cdc_subset.ko
a69fd8d1b4097bcde2e6f97b939bc3d00d485808df643f8945b27df64c2f74e6  dm9601.ko
7e0a520c3b0ff80cd2e10e39c45c3c564d61dec130cf2267137a5051a264eea7  mcs7830.ko
701a2723a7d74ff1d4ae7b875ee062fafb11cd0638729a028c5760726d089462  net1080.ko
abc3d488325d1decf00b1adc05c1497d144319a56f7452e5645c484ffd7db53c  pegasus.ko
e28d0883d535ca4d6b04dc8b84b14705a210b0dd860ae6896354fe16c8e15676  r8152.ko
655d23e3dd90f0b0ed6014767e4d248083cd120ceafc3daf2a4eaa9f5bf1c798  rndis_host.ko
190791015a77ce060e32cd239b48d51bdba87aab1c034093e411a0d289e1d064  rtl8150.ko
7a5035667f726f1c0ad006151148cb734da67d2ebc57d89aa60c39045e8d6197  smsc95xx.ko
d3f1d2470b00bc82c2de511997195b5c03ab472207ed6194396604b7ab770751  usbnet.ko
e11536a78990de142c3aac44d5a5bc639b369a9548ea13fc9714ef59dfa4eee5  zaurus.ko
```

`tests/test_prebuilt_hashes.sh` checks the files against this list. Whoever replaces one of
them updates the list in the same commit, with the source version, the options and the
toolchain used, so the change is visible in the diff.
