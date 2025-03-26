Project 2: Multi-Threaded Network Database Server
Description
This project implements a multi-threaded database server that supports reading, writing, and deleting key-value pairs over a TCP connection. Data is persisted in individual files under /tmp/, and the server supports up to 200 keys with values up to 4096 bytes. In this design update, the work queue implementation from Project 0 has been recycled and integrated into the server to manage incoming client connections efficiently.

Files
proj2.h
Defines the struct request used for communication between clients and the server.

queue.h / queue.c
These files contain a simple, linked-list based queue implementation originally developed for Project 0. The queue is now recycled and used to store work items (client connections) for the server.

dbserver.c
The main multi-threaded database server. Responsibilities include:

Accepting TCP connections on a configurable port (default 5000)

Using a listener thread to accept connections and enqueue them via the recycled queue

Spawning four worker threads to process requests from the queue

Supporting operations: write (W), read (R), delete (D), and quit (Q)

Maintaining an in-memory metadata table for keys and persisting each key's data in /tmp/data.<index>

dbtest.c
A testing utility that sends database requests (set, get, delete, quit, overload, and simultaneous requests) to the server and validates responses. Command-line arguments are parsed with argp.

Makefile
A build script that compiles both dbserver and dbtest with pthread and zlib support. The Makefile now includes the recycled queue files.

test_1.sh
A shell script to automate testing of dbtest. It runs multiple test cases and checks the output against expected results.

Features
Recycled Work Queue:
The queue implementation from Project 0 has been reused to manage client connections. This proven, linked-list based queue decouples the listener and worker threads, ensuring efficient work distribution.

Concurrent Processing:
The server uses a listener thread to accept TCP connections and four worker threads to process them concurrently.

File-based Storage:
Each key is persisted to a separate file (/tmp/data.<index>) which allows keys to contain characters that might otherwise be invalid in filenames.

Command-line Interface:
The server provides a console interface with commands (stats and quit) for monitoring statistics and shutting down gracefully.

Enhanced Testing:
The dbtest utility now supports multiple modes, including individual operations, load testing with multiple threads, simultaneous request testing, and overload testing. An accompanying shell script (test_1.sh) automates these tests and verifies outputs.

Thread Synchronization:
The design employs mutexes and condition variables to protect shared resources, including the work queue (recycled from Project 0) and in-memory key metadata.

Dependencies
Ensure you have the following packages installed:

bash
Copy
sudo apt-get update
sudo apt-get install build-essential libz-dev
Building the Project
Run the following command to compile both the server and the testing utility:

bash
Copy
make
This will produce the following executables:

dbserver – The database server

dbtest – The testing utility

Running the Server
Start the server (default port 5000):

bash
Copy
./dbserver
Or specify a custom port:

bash
Copy
./dbserver 6000
Using the Client (dbtest)
Open a separate terminal and use these commands to interact with the server:

Basic Operations
bash
Copy
./dbtest --set mykey myvalue   # Set (write) a key
./dbtest --get mykey           # Get (read) a key
./dbtest --delete mykey        # Delete a key
./dbtest --quit              # Quit the server
Load Testing
Run a load test with multiple threads and a high request count:

bash
Copy
./dbtest --threads 4 --count 2000
Simultaneous Requests Test
Test with 10 simultaneous requests using 10 threads:

bash
Copy
./dbtest --test --threads 10 --count 500
Overload Testing
Attempt to create more than 200 keys:

bash
Copy
./dbtest --overload
Server Console Commands
In the server terminal, type:

bash
Copy
stats   # Display server statistics
quit    # Shutdown the server
Testing
Automated Testing Script
A shell script (test_1.sh) is provided to run a series of tests. The script executes multiple dbtest commands and checks the output against expected results.

Example usage:

bash
Copy
./test_1.sh
Expected Outputs
The automated tests compare the output of each dbtest invocation with specific expected strings (e.g., "ok", "READ: FAILED (X)", or error messages). This helps ensure that the server behaves as intended under different scenarios.

Implementation Details
Work Queue:
The work queue implementation, recycled from Project 0, is provided by queue.h and queue.c. This queue is used to store client connections as work items, allowing the listener thread to enqueue incoming requests and worker threads to dequeue them in a thread-safe manner.

In-Memory Metadata:
The server maintains an array of key records to track the state (invalid, busy, or valid) and names of stored keys. This metadata is used to quickly locate and manage keys corresponding to file-based storage.

Synchronization:
Mutexes and condition variables are used extensively to protect shared data structures, including the work queue and key metadata, ensuring correct concurrent behavior.

Known Limitations
File writes are not performed atomically, so transactional consistency is not guaranteed.

In-memory metadata is lost when the server is restarted.

The design is limited to a maximum of 200 keys and a single server instance.

Cleanup
To remove compiled binaries and temporary data, run:

bash
Copy
make clean
Recycling the Queue
The queue code used in this project was originally developed for Project 0. By recycling this well-tested implementation, we avoided duplicating functionality and ensured that the multi-threaded work queue is robust and reliable. This reuse not only saves development time but also leverages proven code for efficient management of concurrent client requests.

