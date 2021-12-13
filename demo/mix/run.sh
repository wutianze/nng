#!/usr/bin/env bash

ADDR=tcp://127.0.0.1:8899
CLIENT_COUNT=1
MSG_COUNT=1


./build/server $ADDR &
SERVER_PID=$!
trap "kill $SERVER_PID" 0
typeset -a CLIENT_PID
i=0
sleep 1
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
kill $SERVER_PID
