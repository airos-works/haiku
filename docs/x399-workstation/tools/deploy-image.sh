#!/bin/bash
# Upload an image to the X399 NanoKVM over plain TCP and verify its hash.
# usage: deploy-image.sh <local image> <remote name (.iso/.img)>
set -euo pipefail
src=$1; name=$2
ssh_kvm() { ssh -F /mnt/HaikuWork/x399/ssh/config x399kvm "$@"; }
port=9399
ssh_kvm "rm -f /data/$name.part; python3 -c '
import socket
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((\"0.0.0.0\", $port)); s.listen(1); c,_=s.accept()
f=open(\"/data/$name.part\",\"wb\")
while True:
    b=c.recv(1<<20)
    if not b: break
    f.write(b)
f.flush(); import os; os.fsync(f.fileno()); f.close()
'" &
listener=$!
sleep 2
python3 - "$src" $port <<'PY'
import socket, sys
import time
for attempt in range(60):
    try:
        s = socket.create_connection(("192.168.1.9", int(sys.argv[2])))
        break
    except ConnectionRefusedError:
        time.sleep(1)
else:
    sys.exit("NanoKVM listener did not start")
with open(sys.argv[1], "rb") as f:
    while True:
        b = f.read(1 << 20)
        if not b: break
        s.sendall(b)
s.close()
PY
wait $listener
remote=$(ssh_kvm "mv /data/$name.part /data/$name && sha256sum /data/$name" | cut -d' ' -f1)
local=$(sha256sum "$src" | cut -d' ' -f1)
[ "$remote" = "$local" ] || { echo "hash mismatch" >&2; exit 1; }
echo "uploaded $name $local"
