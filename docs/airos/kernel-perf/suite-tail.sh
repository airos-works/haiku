B64END
chmod +x /tmp/kperf
K=/tmp/kperf
for n in 1 2 4 8; do run $K faultthr $n; done
for n in 1 2 4 8; do run $K faultteam $n; done
for n in 1 4 8; do run $K stat $n; done
for n in 1 8; do run $K areas $n; done
for n in 1 8; do run $K malloc $n; done
run $K anon 512 1
run $K anon 512 8
$K mkfile /boot/home/f64 64
run $K filemap /boot/home/f64
big=$(ls -S /boot/system/lib/*.so* | head -1)
run $K filemap $big
run $K exec 200
run $K daemon 768 30
