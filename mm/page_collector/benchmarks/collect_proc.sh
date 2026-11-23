#! /usr/bin/bash

OUT_DIR=""
MEM_FILE="meminfo.csv"
VM_FILE="vminfo.csv"

function collect_mem() {
	local type="$1"
	local lines=$(cat "/proc/meminfo" |
		grep -E "MemTotal|MemAvaliable|MemFree|Cached|SwapFree")

	if [[ $type == "head" ]]; then
		echo "$lines" | awk 'BEGIN { ORS="," } { sub(/:/,""); print $1}'
	elif [[ $type == "data" ]]; then
		echo "$lines" | awk 'BEGIN { ORS="," } { print $2}'
	fi
}

function collect_vm() {
	local type="$1"
	local lines=$(cat "/proc/vmstat" |
		grep -E "pgfault|pgmajfault")

	if [[ $type == "head" ]]; then
		echo "$lines" | awk 'BEGIN { ORS="," } { print $1}'
	elif [[ $type == "data" ]]; then
		echo "$lines" | awk 'BEGIN { ORS="," } { print $2}'
	fi
}

function write_datas() {
	local file="$1"
	local datas="$2"

	if [[ $file == "mem" ]]; then
		echo "$datas" >>"$OUT_DIR/$MEM_FILE"
	elif [[ $file == "vm" ]]; then
		echo "$datas" >>"$OUT_DIR/$VM_FILE"
	fi
}

function main() {
	while [[ $1 != "" ]]; do
		case "$1" in
		-o | --output)
			OUT_DIR=$2
			shift
			shift
			;;
		*)
			shift
			;;
		esac
	done

	rm -r "$OUT_DIR"
	mkdir -p "$OUT_DIR" &>/dev/null

	write_datas "mem" "$(collect_mem "head")"
	write_datas "vm" "$(collect_vm "head")"

	while [[ true ]]; do
		write_datas "mem" "$(collect_mem "data")"
		write_datas "vm" "$(collect_vm "data")"

		sleep 1
	done
}

main $@
