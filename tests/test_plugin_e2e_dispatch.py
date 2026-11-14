import urllib.request
import json
import time
import subprocess

HUB_IP = "10.234.234.98"

# 1. Fetch current devices
req = urllib.request.Request(f"http://{HUB_IP}:8089/api/devices")
with urllib.request.urlopen(req) as resp:
    data = json.loads(resp.read().decode("utf-8"))
    original_devices = data.get("devices", [])

# 2. Add test device
test_dev = {
    "id": "test-roku-99",
    "name": "Test Roku",
    "type": "plugin:roku-ecp",
    "ip_address": "127.0.0.1",
    "port": 8060,
    "commands": [
        {"name": "Home", "keyCode": "PLUGIN:roku-ecp:Home"},
        {"name": "Select", "keyCode": "PLUGIN:roku-ecp:Select"}
    ]
}

new_devices = [d for d in original_devices if d.get("id") != "test-roku-99"]
new_devices.append(test_dev)

save_req = urllib.request.Request(
    f"http://{HUB_IP}:8089/api/device-save",
    data=json.dumps({"devices": new_devices}).encode("utf-8"),
    headers={"Content-Type": "application/json"},
    method="POST"
)
with urllib.request.urlopen(save_req) as resp:
    print("[+] Device save status:", resp.status)

time.sleep(1)

# 3. Dispatch command to test-roku-99
cmd_req = urllib.request.Request(
    f"http://{HUB_IP}:8089/api/ir-send",
    data=json.dumps({"deviceId": "test-roku-99", "command": "Home", "action": "tap"}).encode("utf-8"),
    headers={"Content-Type": "application/json"},
    method="POST"
)
try:
    with urllib.request.urlopen(cmd_req) as resp:
        print("[+] Device command status:", resp.status, resp.read().decode("utf-8"))
except Exception as e:
    print("[-] Device command error:", e)

time.sleep(1)

# 4. Check Hub log via SSH
ssh_cmd = [
    "ssh.exe", "-i", "C:\\Users\\YR\\.ssh\\harmony_owner_ed25519", f"root@{HUB_IP}",
    "cat /tmp/plugins/roku-ecp.log"
]
res = subprocess.run(ssh_cmd, capture_output=True, text=True)
print("\n=== Hub roku-ecp.log ===")
print(res.stdout)

# 5. Clean up test device
cleanup_req = urllib.request.Request(
    f"http://{HUB_IP}:8089/api/device-save",
    data=json.dumps({"devices": original_devices}).encode("utf-8"),
    headers={"Content-Type": "application/json"},
    method="POST"
)
with urllib.request.urlopen(cleanup_req) as resp:
    print("[+] Cleanup device save status:", resp.status)
