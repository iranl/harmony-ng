# Prebuilt binaries kept in the repository

These files are committed as binaries. No build script in the repository produces them, and
the release ships them as they are, so they cannot be rebuilt and compared today. This file
records what is known about each one, so a change to any of them shows up in the diff.

| File | What it is | SHA-256 | Notes |
|---|---|---|---|
| `tools/bin/dropbearmulti_mips` | Hub SSH server (multi-call: dropbear, dropbearkey, scp) | `65c53c9534be562c0335138489ddd5712782898f1c93e85608f393dc4f54604f` | MIPS32 big-endian, static, 303,628 bytes. Reports `SSH-2.0-dropbear_2026.94`. Used to root a Hub on the stock firmware (`tools/root_hub_usb.py`, `tools/unbrick_recovery`); the Linux 6.18 root file system has its own dynamic build. Built by `build/build_minimal_dropbear.sh` (dropbear 2026.94 from a tarball it expects in `build/tmp/dropbear/`, Bootlin `mips32--uclibc--stable-2017.05` toolchain; the script holds the options: Ed25519, curve25519 and chacha20-poly1305 only, no password auth, no TCP or stream forwarding, no client: `dbclient` is not in it) and copied here by hand. |
| `remote/bin/dropbearmulti_musl` | Remote SSH server | `d8b1ce9a8df2003a85915641bffe0f6a329536adc7ccab860744c2f9b361d868` | ARM EABI5, static-pie (musl). Reports `SSH-2.0-dropbear_2024.85`. No build script. |
| `remote/b64dec_tiny` | Small base64 decoder used while rooting the remote | `0a21a5d8aa3760b6d9e149bead49e6f66acdd236935381fa36029374bda08c94` | Its source is `remote/b64dec_tiny.c`; no build script compiles it. |
| `tools/unbrick_recovery/recv_static` | MIPS program kept with the Hub recovery tools, named after the `/tmp/recv` file receiver | `1efbdadfed6ddc2408086d3cbbc069f46f47a637779ff32e5681c2825c2dcf0b` | MIPS32 big-endian, static, 99,956 bytes. Its compiler string is `GCC: (Buildroot 2017.05) 5.4.0`. No source and no build script in the repository, and no guide, tool or script refers to it. Not in the release bundles. |
| `tools/unbrick_recovery/upload_recv.sh` | Script pasted into the Hub's serial shell: it writes a small TCP file receiver to `/tmp/recv` | `5328eb86a9f9c561b525033440e37608322a4cd9aa2b7fa3410f679b8cbf4b27` | The hash is that of the script. The receiver is inside it as 68 `echo -ne` lines: MIPS32 big-endian, linked against the Hub's uClibc, 4,332 bytes, SHA-256 `1f9a076f4c488679e3aaa3121d31ab39de62273176ba9af458a7a5a6decf06bc` once written. No source in the repository. Used by `docs/OFFLINE_UNBRICK_AND_ROOT_GUIDE.md`. Not in the release bundles. |

`tests/test_prebuilt_hashes.sh` checks the files against this list. Whoever replaces one of
them updates the list in the same commit, with the source version, the options and the
toolchain used, so the change is visible in the diff.
