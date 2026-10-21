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
    text_exts = ('.html', '.js', '.css', '.svg', '.json', '.xml', '.txt')

    idx_path = os.path.join(www_dir, "index.html")
    if os.path.isfile(idx_path):
        targets.append(idx_path)

    assets_dir = os.path.join(www_dir, "assets")
    if os.path.isdir(assets_dir):
        for root, _, files in os.walk(assets_dir):
            for file in files:
                if file.endswith(text_exts) and not file.endswith(".gz"):
                    targets.append(os.path.join(root, file))

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
        # Delete uncompressed original to save flash space
        os.remove(target)
        saved = 100.0 - (len(gz_data) / max(len(data), 1) * 100.0)
        print(f"  [+] Gzipped & replaced {os.path.basename(target)}: {len(data):,} -> {len(gz_data):,} bytes (saved {saved:.1f}%)")

    # Remove any useless .png.gz or image .gz
    for root, _, files in os.walk(www_dir):
        for file in files:
            if (file.endswith('.png.gz') or file.endswith('.jpg.gz') or file.endswith('.ico.gz')):
                os.remove(os.path.join(root, file))

    print(f"[+] Done! {len(targets)} files compressed into .gz (raw files removed). Flash savings: {total_orig - total_gz:,} bytes.")

if __name__ == "__main__":
    main()
