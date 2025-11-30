#!/bin/bash

set -e

echo "[+] Atualizando pacotes..."
sudo apt update -y

echo "[+] Instalando stress-ng e utilitários..."
sudo apt install -y stress-ng htop iotop sysstat curl wget

echo "[+] Instalando Redis..."
sudo apt install -y redis-server

echo "[+] Instalando Memcached..."
sudo apt install -y memcached libmemcached-tools

echo "[+] Instalando PostgreSQL..."
sudo apt install -y postgresql postgresql-contrib

echo "[+] Instalando Nginx..."
sudo apt install -y nginx

echo "[✓] Dependências instaladas com sucesso!"

