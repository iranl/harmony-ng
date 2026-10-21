import os
import subprocess
import sys

key_path = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/.ssh/harmony_owner_ed25519")

# Write clean /etc/profile with LF endings
profile_content = b"""# /etc/profile: system-wide .profile file for Bourne compatible shells
PATH="/data/bin:/usr/local/bin:/usr/bin:/bin"
EDITOR="/bin/vi"
test -z "$TERM" && TERM="vt100"

if [ ! -e /etc/localtime ]; then
\tTZ="UTC"
\texport TZ
fi

if [ "$HOME" = "/home/root" ]; then
   PATH=$PATH:/usr/local/sbin:/usr/sbin:/sbin:
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

profile_content = profile_content.replace(b"\r\n", b"\n").replace(b"\r", b"\n")

p = subprocess.Popen(
    ["ssh", "-i", key_path, "-o", "StrictHostKeyChecking=no", "root@192.168.2.1", "cat > /etc/profile && rm -f /home/root/.profile"],
    stdin=subprocess.PIPE
)
p.communicate(profile_content)
print("Updated /etc/profile with /data/bin, exit:", p.returncode)
