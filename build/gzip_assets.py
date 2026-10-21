#!/usr/bin/env python3
import gzip
import os
import sys

def main():
    if len(sys.argv) < 2:
        print("Usage: gzip_assets.py <www_dir>")
        sys.exit(1)

    www_dir = os.path.abspath(sys.argv[1])
    if not os.path.isdir(www_dir):
        print(f"Error: {www_dir} is not a directory")
        sys.exit(1)

    targets = []
    idx_path = os.path.join(www_dir, "index.html")
    if os.path.isfile(idx_path):
        targets.append(idx_path)

    assets_dir = os.path.join(www_dir, "assets")
    if os.path.isdir(assets_dir):
        for entry in os.listdir(assets_dir):
            if not entry.endswith(".gz"):
                full_path = os.path.join(assets_dir, entry)
                if os.path.isfile(full_path):
                    targets.append(full_path)

    total_orig = 0
    total_gz = 0
    for target in targets:
        with open(target, "rb") as f_in:
            data = f_in.read()
        gz_data = gzip.compress(data, 9)
        gz_path = target + ".gz"
        with open(gz_path, "wb") as f_out:
            f_out.write(gz_data)
        total_orig += len(data)
        total_gz += len(gz_data)
        print(f"  [+] Gzipped {os.path.basename(target)}: {len(data):,} -> {len(gz_data):,} bytes (saved {100.0 - (len(gz_data) / max(len(data), 1) * 100.0):.1f}%)")

    print(f"[+] Done! {len(targets)} files compressed. Total: {total_orig:,} -> {total_gz:,} bytes.")

if __name__ == "__main__":
    main()
