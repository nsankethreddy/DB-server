#! /bin/bash

for i in {1..201}; do
    ./dbtest -p 5001 -S "overload_key$i" "value" &
done
wait
