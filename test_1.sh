#!/bin/bash

check_command() {
    expected="$1"
    shift
    # Run the command, capture its output, replace newlines with spaces, then trim whitespace.
    output=$("$@" 2>&1 | tr '\n' ' ' | sed 's/^ *//;s/ *$//')
    if [ "$output" = "$expected" ]; then
        echo "Test passed for: $*"
    else
        echo "Test failed for: $*"
        echo "Expected: '$expected'"
        echo "Got:      '$output'"
    fi
}

check_command "" ./dbtest
check_command "" ./dbtest --threads 4 --count 2000
check_command "" ./dbtest --test --threads 10 --count 500
check_command "READ: FAILED (X)" ./dbtest --get mykey
check_command "ok" ./dbtest --set mykey myvalue
check_command '="myvalue"' ./dbtest --get mykey
check_command "ok" ./dbtest --delete mykey
check_command "READ: FAILED (X)" ./dbtest --get mykey
check_command "" ./dbtest --quit
check_command "can't connect: Connection refused" ./dbtest --overload
