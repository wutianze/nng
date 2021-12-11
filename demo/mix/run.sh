#!/usr/bin/env bash

ADDR=tcp://127.0.0.1:8899
COUNT=10

./server $ADDR &
SERVER_PID=$!
trap "kill $SERVER_PID" 0
typeset -a CLIENT_PID
i=0
sleep 1
while (( i < COUNT ))
do
	i=$(( i + 1 ))
	rnd=$(( RANDOM % 1000 + 500 ))
	echo "Starting client $i: server replies after $rnd msec"
	./client $ADDR $rnd &
	eval CLIENT_PID[$i]=$!
done

i=0
while (( i < COUNT ))
do
	i=$(( i + 1 ))
	wait ${CLIENT_PID[$i]}
done
kill $SERVER_PID
