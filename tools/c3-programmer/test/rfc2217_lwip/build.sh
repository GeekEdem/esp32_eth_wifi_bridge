#!/bin/bash
# Builds the host harness against lwIP from $IDF_PATH (unix port).
# Usage: build.sh <out> [rfc2217_server.c] [SO_LINGER 0/1]
set -e
cd "$(dirname "$0")"
OUT=$1; LIB=${2:-../../components/rfc2217-server/src/rfc2217_server.c}; LINGER=${3:-1}
L=$IDF_PATH/components/lwip/lwip
U=$L/contrib/ports/unix/port
INC="-DTEST_SO_LINGER=$LINGER -Iinclude -I$L/src/include -I$U/include"
T=$(mktemp -d); trap 'rm -rf $T' EXIT
# lwIP and its tap driver see the host's own headers ...
for f in $L/src/core/*.c $L/src/core/ipv4/*.c $L/src/api/*.c $L/src/netif/ethernet.c $U/sys_arch.c $U/netif/tapif.c; do
    cc -O1 -g -w $INC -c "$f" -o $T/$(basename "$f" .c).o
done
# ... the library and the harness get lwIP's sockets for <sys/socket.h> etc.
cc -O1 -g -Wall -Wno-unused-parameter -Isockets $INC -I../../main -I../../components/rfc2217-server/include \
   -o "$OUT" harness.c ../../main/client_watch.c "$LIB" $T/*.o -lpthread
