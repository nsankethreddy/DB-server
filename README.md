# Project 2: Multi-Threaded Network Database Server

## Description
This project implements a simple multi-threaded **database server** that supports reading, writing, and deleting key-value pairs over a **TCP connection**. The database is backed by individual files stored in `/tmp/`, and supports **up to 200 keys** with values up to **4096 bytes**.

The server spawns **multiple worker threads** to process client requests concurrently and maintains a **work queue** for incoming connections. It also provides a command-line interface to monitor database statistics and shut down the server.

## Files

- **proj2.h**  
  Defines the `struct request` used for communication between client and server.
  
- **dbtest.c**  
  A testing utility that sends database requests (`set`, `get`, `delete`, etc.) to the server and validates responses.
  
- **dbserver.c**  
  The main **multi-threaded database server**, responsible for:
  - Managing **TCP connections**
  - Handling **requests (read, write, delete, quit)**
  - Managing **a worker thread pool**
  - Maintaining **in-memory metadata** of stored keys
  - Writing and reading data from **individual files**
  
- **Makefile**  
  A build script to compile both `dbserver` and `dbtest` with **pthread** and **zlib** support.
  
## Features

- **Supports concurrent client connections** using a **listener thread** and **four worker threads**.
- **TCP-based database** with commands to **write, read, and delete keys**.
- **In-memory metadata** for fast lookups while persisting data to disk.
- **File-based storage** using `/tmp/data.<index>` for each key.
- **Thread synchronization** using **mutexes and condition variables**.
- **Database statistics** via `stats` command.
- **Graceful shutdown** via `quit` command.

## Dependencies

Ensure the following packages are installed:

```bash
sudo apt-get update
sudo apt-get install build-essential libz-dev
```

## Building the Project

To compile both the server and the test utility, run:
```bash
make
```
This will generate the following binaries:
- `dbserver` - The database server
- `dbtest` - The testing utility

## Running the Server

Start the database server (default port **5000**):
```bash
./dbserver
```
Or specify a custom port:
```bash
./dbserver 6000
```

## Using the Client (`dbtest`)

Run `dbtest` in a separate terminal to interact with the server.

### Basic Commands
```bash
./dbtest -S mykey myvalue   # Set (write) a key
./dbtest -G mykey           # Get (read) a key
./dbtest -D mykey           # Delete a key
./dbtest -q                 # Quit the server
```

### Load Testing
Run multiple concurrent requests:
```bash
./dbtest -n 5000 -t 5       # 5000 requests using 5 threads
```

### Simultaneous Requests Test
```bash
./dbtest -T
```

## Server Console Commands

In the **server terminal**, type:
```bash
stats   # Show server statistics
quit    # Shutdown the server
```

## Implementation Details

- **Database Structure**: Uses an in-memory table (`g_table[MAX_KEYS]`) to track keys and their file locations.
- **Threading Model**:
  - **Listener thread**: Accepts TCP connections and queues requests.
  - **Worker threads**: Process requests from the queue.
- **Synchronization**: Uses `pthread_mutex_t` to protect shared resources.
- **Work Queue**: Maintains a queue of client requests to distribute work among threads.
- **File Storage**: Keys are mapped to `/tmp/data.<index>`, ensuring safe persistence.
- **Error Handling**: Server handles invalid requests and failed writes, returning an error (`X`).

## Known Limitations

- No **transactional consistency** (files are written without atomic commits).
- No **persistence after restart** (in-memory metadata is lost).
- **Limited scalability** (fixed to 200 keys, single server instance).

## Testing Script (`testing.sh`)

For automated testing, create a `testing.sh` script:
```bash
#!/bin/bash
./dbserver &
SERVER_PID=$!
sleep 2  # Wait for server to start

# Run tests
./dbtest -S key1 value1
./dbtest -G key1
./dbtest -D key1
./dbtest -T

# Shutdown server
echo "quit" | nc localhost 5000
wait $SERVER_PID
```
Make it executable:
```bash
chmod +x testing.sh
```
Run it:
```bash
./testing.sh
```

## Cleanup

To remove compiled files and temporary data:
```bash
make clean
```


