import os
import subprocess
import sys

key_path = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/.ssh/harmony_owner_ed25519")

profile_content = """# /etc/profile
PATH="/data/bin:/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin"
export PATH
EDITOR="/bin/vi"
test -z "$TERM" && TERM="vt100"
if [ ! -e /etc/localtime ]; then
    TZ="UTC"
    export TZ
fi
if [ "$PS1" ]; then
   PS1='\\u@\\h:\\w\\$ '
fi
if [ -d /etc/profile.d ]; then
  for i in /etc/profile.d/* ; do
    . $i
  done
  unset i
fi
export PATH PS1 EDITOR TERM
umask 022
"""

home_profile = """PATH="/data/bin:$PATH"
export PATH
"""

ssh_base = ["ssh", "-i", key_path, "-o", "StrictHostKeyChecking=no", "root@192.168.2.1"]

p1 = subprocess.Popen(ssh_base + ["cat > /etc/profile"], stdin=subprocess.PIPE, text=True)
p1.communicate(profile_content)

p2 = subprocess.Popen(ssh_base + ["cat > /home/root/.profile"], stdin=subprocess.PIPE, text=True)
p2.communicate(home_profile)

print("Profiles written successfully")
