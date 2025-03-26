#! /bin/bash
for i in {1..100}; do
    ./dbtest -p 5000 -S "key$i" "value$i" &
done
wait
