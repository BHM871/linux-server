#!/bin/bash

LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

echo "[+] Starting PostgreSQL..."
sudo systemctl start postgresql
sleep 2

echo "[+] Preparing banco pgbench..."
sudo -u postgres pgbench -i -s 10 postgres

echo "[+] Executing pgbench..."
sudo -u postgres pgbench -c 10 -t 100000 postgres |
	tee "$LOGDIR/postgres_bench.log"

sudo systemctl stop postgresql

echo "[✓] PostgreSQL benchmark finished!"
