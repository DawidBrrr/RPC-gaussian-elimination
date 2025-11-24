#!/bin/bash

# Skrypt do kompilacji serwera i klienta RPC
echo "Kompilowanie serwera..."
g++ -I/usr/include/tirpc -o gaus_server src/gaus_server.cpp src/gaus_rpc_svc.c src/gaus_rpc_xdr.c -ltirpc

echo "Kompilowanie klienta..."

g++ -I/usr/include/tirpc -o gaus_client src/gaus_client.cpp src/gaus_rpc_clnt.c src/gaus_rpc_xdr.c -ltirpc

echo "Kompilacja zakończona!"