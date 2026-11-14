import base64
import urllib.request
import json
import time

HUB_IP = "10.234.234.98"
PLUGINS = ["roku-ecp", "ps5-wake", "android-tv"]

print(f"=== Installing and Verifying Plugins on Hub {HUB_IP} ===")
for p in PLUGINS:
    tar_path = f"../harmony-ng-plugins/dist/{p}-plugin.tar.gz"
    with open(tar_path, "rb") as f:
        b64 = base64.b64encode(f.read()).decode("ascii")
    req = urllib.request.Request(
        f"http://{HUB_IP}:8089/api/plugins/install",
        data=json.dumps({"archive_base64": b64}).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST"
    )
    with urllib.request.urlopen(req) as resp:
        res = json.loads(resp.read().decode("utf-8"))
        print(f"[+] Installed {p}: {res.get('message', res)}")

time.sleep(1)
req_list = urllib.request.Request(f"http://{HUB_IP}:8089/api/plugins")
with urllib.request.urlopen(req_list) as resp:
    data = json.loads(resp.read().decode("utf-8"))
    installed = data.get("plugins", [])
    print(f"\n[+] Total Installed Plugins: {len(installed)}")
    for pl in installed:
        print(f"    -> {pl['id']} (v{pl.get('version')}): running={pl.get('running')} pid={pl.get('pid')}")

print("\n=== Verifying Process and Sockets on Hub ===")
