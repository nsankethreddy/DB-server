# Project 2: Multi-Threaded Network Database Server

## Description
This project implements a multi-threaded database server that supports reading, writing, and deleting key-value pairs over a TCP connection. Data is persisted in individual files under `/tmp/`, and the server supports up to 200 keys with values up to 4096 bytes. In this design update, the work queue implementation from Project 0 has been recycled and integrated into the server to manage incoming client connections efficiently.

## Files

- **proj2.h**  
  Defines the `struct request` used for communication between clients and the server.

- **queue.h / queue.c**  
  These files contain a simple, linked-list based queue implementation originally developed for Project 0. The queue is now recycled and used to store work items (client connections) for the server.

- **dbserver.c**  
  The main multi-threaded database server. Responsibilities include:
  - Accepting TCP connections on a configurable port (default 5000)
  - Using a listener thread to accept connections and enqueue them via the recycled queue
  - Spawning four worker threads to process requests from the queue
  - Supporting operations: write (W), read (R), delete (D), and quit (Q)
  - Maintaining an in-memory metadata table for keys and persisting each key's data in `/tmp/data.<index>`

- **dbtest.c**  
  A testing utility that sends database requests (set, get, delete, quit, overload, and simultaneous requests) to the server and validates responses. Command-line arguments are parsed with `argp`.

- **Makefile**  
  A build script that compiles both `dbserver` and `dbtest` with pthread and zlib support. The Makefile includes the recycled queue files.

- **test_1.sh**  
  A shell script to automate testing of `dbtest`. It runs multiple test cases and checks the output against expected results.
  The script tests several things from basic read,write and delete operations to various modes that dbtest can be run on including
  overload mode and test mode. To run the script first start dbserver then run ./test_1.sh

## Features

- **Recycled Work Queue:**  
  The queue implementation from Project 0 has been reused to manage client connections. This proven, linked-list based queue decouples the listener and worker threads, ensuring efficient work distribution.

- **File-based Storage:**  
  Each key is persisted to a separate file (`/tmp/data.<index>`) which allows keys to contain characters that might otherwise be invalid in filenames.


- **Thread Synchronization:**  
  Mutexes and condition variables are used extensively to protect shared resources, including the work queue (recycled from Project 0) and in-memory key metadata.

## Dependencies
Ensure you have the following packages installed:

```bash
sudo apt-get update
sudo apt-get install build-essential libz-dev
