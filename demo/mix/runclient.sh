#!/usr/bin/env bash

ADDR=tcp://192.168.3.101:8899
CLIENT_COUNT=10
MSG_COUNT=10

i=0
while (( i < CLIENT_COUNT ))
do
	i=$(( i + 1 ))
	rnd=$(( RANDOM % 1000 + 500 ))
	echo "Starting client $i: server replies after $rnd msec"
	./build/client $ADDR $rnd $MSG_COUNT &
	eval CLIENT_PID[$i]=$!
done

i=0
while (( i < CLIENT_COUNT ))
do
	i=$(( i + 1 ))
	wait ${CLIENT_PID[$i]}
done
