#! /usr/bin/env bash

EXECUT="workload"
LOGDIR="./bench_logs"
mkdir -p "$LOGDIR"

if [[ ! -f "./$EXECUT" ]]; then
	echo "[...] Compilando workload..."
	gcc "./$EXECUT.c" -o "./$EXECUT" 2>/dev/null
fi

echo "[+] Iniciando esquenta do workload controlado..."
./$EXECUT 2>/dev/null

rm "$LOGDIR/$EXECUT.log"

echo "[+] Iniciando workload controlado..."
for _ in {1..10}; do
	./$EXECUT | tee "$LOGDIR/$EXECUT-tmp.log"
	cat "$LOGDIR/$EXECUT-tmp.log" >>"$LOGDIR/$EXECUT.log"
done

rm "$LOGDIR/$EXECUT-tmp.log"

echo "[✓] Workload finalizado!"
