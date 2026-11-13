B64END
chmod +x /tmp/kperf
K=/tmp/kperf
run $K large 512
run $K large 512
log "KP_INFO $(sysinfo -mem 2>/dev/null | tr '\n' ' ')"
run $K anon 512 1
run $K verify 1024 3
