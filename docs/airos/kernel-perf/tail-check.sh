B64END
chmod +x /tmp/kperf
K=/tmp/kperf
run $K mapwrite /boot/home/mw 64
run $K pressure 1100 900 /boot/home/pf 6
log "KP_INFO $(sysinfo -mem 2>/dev/null | tr '\n' ' ')"
run $K daemon 768 30
