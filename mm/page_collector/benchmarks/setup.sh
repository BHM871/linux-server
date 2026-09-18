#!/bin/bash

set -e

echo "[+] Compilando workload..."
gcc workload.c -o workload

echo "[+] Instalando stress-ng e utilitários..."
sudo pacman -S stress-ng htop iotop sysstat curl wget

echo "[+] Instalando Memcached..."
sudo pacman -S memcached

echo "[+] Instalando PostgreSQL..."
sudo pacman -S postgresql

echo "[+] Instalando Nginx..."
sudo pacman -S nginx siege

echo "[✓] Dependências instaladas com sucesso!"
