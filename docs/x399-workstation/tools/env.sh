# Source for X399 workstation work. Keeps all writes on /mnt/HaikuWork.
export X399=/mnt/HaikuWork/x399
export TMPDIR=/mnt/HaikuWork/tmp TMP=/mnt/HaikuWork/tmp TEMP=/mnt/HaikuWork/tmp
export XDG_CACHE_HOME=/mnt/HaikuWork/cache CCACHE_DIR=/mnt/HaikuWork/cache/ccache
export PATH="/mnt/HaikuWork/toolchains/bin:/mnt/HaikuWork/toolchains/host/usr/bin:$PATH"
export NANOKVM_URL=http://192.168.1.9 NANOKVM_SESSION=$X399/state/nanokvm-session.json
alias kvm="/mnt/HaikuWork/nanokvm/.venv/bin/python $X399/tools/kvm.py"
