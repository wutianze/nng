./pair node0 tcp://0.0.0.0:8899 & node0=$!
./pair node1 tcp://192.168.3.102:8899 & node1=$!
sleep 5
kill $node0 $node1
