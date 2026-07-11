#!/bin/bash

set -e

gcc -o cliente cliente.c
gcc -o servidor servidor.c
docker cp cliente term3:/cliente
docker cp servidor term4:/servidor

docker cp its_gateway_rs.c term1:/its_gateway_rs.c
docker cp its_gateway_rq_istsrc.c term1:/its_gateway_rq_istsrc.c
docker cp gw_exporter_ping.py term1:/gw_exporter_ping.py
docker cp ground_station_rs.c term2:/ground_station_rs.c
docker cp ground_station_rq_istsrc.c term2:/ground_station_rq_istsrc.c
docker exec term1 gcc -o its_gateway_rs its_gateway_rs.c -I/usr/include/libnetfilter_queue -I/usr/include/libnfnetlink -lnetfilter_queue -lnfnetlink -lfec
docker exec term1 gcc -o its_gateway_rq its_gateway_rq_istsrc.c -I/usr/include/libnetfilter_queue -I/usr/include/libnfnetlink -lnetfilter_queue -lnfnetlink -llcrq
docker exec term2 gcc -o ground_station_rs ground_station_rs.c -lfec
docker exec term2 gcc -o ground_station_rq ground_station_rq_istsrc.c -llcrq

docker exec term1 iptables -I FORWARD -p udp --dport 12345 -j NFQUEUE --queue-num 1
#sysctl -w net.netfilter.nf_queue_maxlen=32768
sysctl -w net.core.wmem_max=4194304
