#!/bin/bash

LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

echo "[+] Iniciando stress-ng para gerar page faults..."
stress-ng --vm 4 --vm-bytes 4G --vm-method all --timeout 180s \
    --verify --metrics-brief | tee "$LOGDIR/stress-ng.log"

echo "[✓] Stress-ng finalizado!"

