#!/bin/bash

set -e

echo "[+] Instalando stress-ng e utilitários..."
sudo pacman -S stress-ng htop iotop sysstat curl wget

echo "[+] Instalando Memcached..."
sudo pacman -S memcached

echo "[+] Instalando PostgreSQL..."
sudo pacman -S postgresql

echo "[+] Instalando Nginx..."
sudo pacman -S nginx

echo "[✓] Dependências instaladas com sucesso!"

