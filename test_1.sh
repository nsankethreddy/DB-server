#!/bin/bash
# Run dbtest with default options
./dbtest > /dev/null 2>&1

# Run with 4 threads and 2000 requests total
./dbtest --threads 4 --count 2000 > /dev/null 2>&1

# Run in test mode with 10 threads and 500 requests per thread (10 simultaneous requests)
./dbtest --test --threads 10 --count 500 > /dev/null 2>&1

# Send a set command: set key "mykey" to value "myvalue"
./dbtest --set mykey myvalue > /dev/null 2>&1

# Send a get command: retrieve the value for "mykey"
./dbtest --get mykey > /dev/null 2>&1

# Send a delete command: delete key "mykey"
./dbtest --delete mykey > /dev/null 2>&1

# Send a quit command to the server
./dbtest --quit > /dev/null 2>&1

# Run overload mode to attempt creating more than 200 keys
./dbtest --overload > /dev/null 2>&1
