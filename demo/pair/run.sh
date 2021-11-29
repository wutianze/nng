./pair node0 tcp://127.0.0.1:8899 & node0=$!
./pair node1 tcp://127.0.0.1:8899 & node1=$!
sleep 5
kill $node0 $node1
