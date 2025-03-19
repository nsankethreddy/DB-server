#! /bin/bash
for i in {1..100}; do
    ./dbtest -p 5001 -S "key$i" "value$i" &
done
wait
