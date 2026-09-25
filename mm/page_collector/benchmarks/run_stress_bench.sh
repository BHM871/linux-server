#!/bin/bash

LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

echo "[+] Starting stress-ng to cause page faults..."
stress-ng --vm 8 --vm-bytes 8G --vm-method all --timeout 300s \
	--verify --metrics-brief | tee "$LOGDIR/stress-ng.log"

echo "[✓] Stress-ng finished!"
