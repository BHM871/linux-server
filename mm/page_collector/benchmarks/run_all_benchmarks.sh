#!/bin/bash

log=1

if [ "$1" == "" ]; then
	echo "[x] Precisa especificar o modulo"
	exit 1
elif [[ "$1" == "none" ]]; then
	log=0
fi

PROCFILE="/proc/$1"

LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

echo "[+] Limpando logs antigos..."
rm -f $LOGDIR/*

if [[ $log -eq 1 ]]; then
	echo "[+] Registrando estado inicial do módulo..."
	cat $PROCFILE | sort -n >"$LOGDIR/inicial_proc_dump.csv"
fi

start=$(date +%s%N)

echo "[...] Rodando Workload..."
./run_workload.sh

echo "[...] Rodando stress-ng..."
./run_stress_bench.sh

echo "[...] Rodando Memcached benchmark..."
./run_memcached_bench.sh

echo "[...] Rodando PostgreSQL benchmark..."
./run_postgres_bench.sh

echo "[...] Rodando Nginx benchmark..."
./run_nginx_bench.sh

end=$(date +%s%N)
duration=$((end - start))

if [[ $log -eq 1 ]]; then
	echo "[+] Capturando logs finais do módulo..."
	cat $PROCFILE | sort -n >"$LOGDIR/final_proc_dump.csv"
	echo "# timers start: $start end: $end duration: $duration" >>"$LOGDIR/final_proc_dump.csv"
fi

echo "[+] start: $start end: $end duration: $duration"

echo "[✓] Todos benchmarks concluídos!"

if [[ $log -eq 1 ]]; then
	echo "Resultados armazenados em $LOGDIR/"
fi
