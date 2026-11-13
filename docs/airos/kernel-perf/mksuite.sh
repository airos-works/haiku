#!/bin/bash
# mksuite.sh <out> [tail]: suite script with kperf embedded
cd /data1/scratch/kperf
{ cat suite-head.sh; base64 kperf; cat ${2:-suite-tail.sh}; } > $1
