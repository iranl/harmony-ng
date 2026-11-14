import urllib.request
import json
import time
import subprocess

HUB_IP = "10.234.234.98"

print("=== 1. Testing roku-ecp ===")
dev_roku = {
    "id": "test-roku",
    "name": "Roku Test",
    "type": "plugin:roku-ecp",
    "plugin": "roku-ecp",
    "ip_address": "192.168.1.50",
    "port": 8060,
    "commands": [{"name": "Home", "keyCode": "PLUGIN:roku-ecp:Home"}]
}
urllib.request.urlopen(urllib.request.Request(f"http://{HUB_IP}:8089/api/device-save", data=json.dumps(dev_roku).encode("utf-8"), headers={"Content-Type": "application/json"}, method="POST"))
urllib.request.urlopen(urllib.request.Request(f"http://{HUB_IP}:8089/api/ir-send", data=json.dumps({"deviceId": "test-roku", "command": "Home", "action": "tap"}).encode("utf-8"), headers={"Content-Type": "application/json"}, method="POST"))

print("=== 2. Testing ps5-wake ===")
dev_ps5 = {
    "id": "test-ps5",
    "name": "PS5 Test",
    "type": "plugin:ps5-wake",
    "plugin": "ps5-wake",
    "host_mac": "AA:BB:CC:DD:EE:FF",
    "commands": [{"name": "Wake", "keyCode": "PLUGIN:ps5-wake:Wake"}]
}
urllib.request.urlopen(urllib.request.Request(f"http://{HUB_IP}:8089/api/device-save", data=json.dumps(dev_ps5).encode("utf-8"), headers={"Content-Type": "application/json"}, method="POST"))
urllib.request.urlopen(urllib.request.Request(f"http://{HUB_IP}:8089/api/ir-send", data=json.dumps({"deviceId": "test-ps5", "command": "Wake", "action": "tap"}).encode("utf-8"), headers={"Content-Type": "application/json"}, method="POST"))

print("=== 3. Testing android-tv ===")
dev_atv = {
    "id": "test-atv",
    "name": "ATV Test",
    "type": "plugin:android-tv",
    "plugin": "android-tv",
    "host": "192.168.1.60",
    "port": 6467,
    "commands": [{"name": "Home", "keyCode": "PLUGIN:android-tv:Home"}]
}
urllib.request.urlopen(urllib.request.Request(f"http://{HUB_IP}:8089/api/device-save", data=json.dumps(dev_atv).encode("utf-8"), headers={"Content-Type": "application/json"}, method="POST"))
urllib.request.urlopen(urllib.request.Request(f"http://{HUB_IP}:8089/api/ir-send", data=json.dumps({"deviceId": "test-atv", "command": "Home", "action": "tap"}).encode("utf-8"), headers={"Content-Type": "application/json"}, method="POST"))

time.sleep(1)

ssh_cmd = ["ssh.exe", "-i", "C:\\Users\\YR\\.ssh\\harmony_owner_ed25519", f"root@{HUB_IP}", "cat /tmp/plugins/*.log"]
res = subprocess.run(ssh_cmd, capture_output=True, text=True)
print("\n=== Live Logs from Hub /tmp/plugins/*.log ===")
print(res.stdout)

# Clean up
for d in ["test-roku", "test-ps5", "test-atv"]:
    try:
        req = urllib.request.Request(f"http://{HUB_IP}:8089/api/device-delete?id={d}", method="POST")
        urllib.request.urlopen(req)
    except Exception:
        pass
print("[+] Cleanup complete!")
