#!/bin/bash

set -e

echo "[+] Atualizando pacotes..."
sudo pacman -Su

echo "[+] Instalando stress-ng e utilitários..."
sudo pacman -S stress-ng htop iotop sysstat curl wget

echo "[+] Instalando Redis..."
sudo pacman -S redis-server

echo "[+] Instalando Memcached..."
sudo pacman -S memcached libmemcached-tools

echo "[+] Instalando PostgreSQL..."
sudo pacman -S postgresql postgresql-contrib

echo "[+] Instalando Nginx..."
sudo pacman -S nginx

echo "[✓] Dependências instaladas com sucesso!"

