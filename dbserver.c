#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>

#include "proj2.h"

/* --- Project constraints --- */
#define MAX_KEYS 200
#define STATE_INVALID 0
#define STATE_BUSY 1
#define STATE_VALID 2

/* --- Data for each key --- */
typedef struct
{
    char name[32]; // the key (up to 30 chars + null)
    int state;     // 0=INVALID, 1=BUSY, 2=VALID
} KeyRecord;
int g_listen_fd = -1;

static KeyRecord g_keys[MAX_KEYS]; // Our "table": up to 200 entries
static pthread_mutex_t g_keys_lock = PTHREAD_MUTEX_INITIALIZER;

/* --- Stats & state tracking --- */
static int g_num_objects = 0; // how many keys are in VALID state
static int g_num_write = 0;   // count of W requests
static int g_num_read = 0;    // count of R requests
static int g_num_delete = 0;  // count of D requests
static int g_num_fail = 0;    // count of 'X' responses
static int g_num_queued = 0;  // requests waiting in queue

static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;

/* A global to signal shutdown. Worker threads & listener will stop if set. */
static int g_shutdown_flag = 0;

/* --- Work queue for incoming requests from the listener --- */
typedef struct WorkItem
{
    int client_fd;
    struct WorkItem *next;
} WorkItem;

static WorkItem *g_work_head = NULL;
static WorkItem *g_work_tail = NULL;
static pthread_mutex_t g_work_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work_cond = PTHREAD_COND_INITIALIZER;

/******************************************************************************
 * Enqueue a new socket onto the shared queue.
 *****************************************************************************/
static void queue_work(int fd)
{
    WorkItem *item = malloc(sizeof(*item));
    if (!item)
    {
        fprintf(stderr, "queue_work: out of memory!\n");
        return;
    }
    item->client_fd = fd;
    item->next = NULL;

    pthread_mutex_lock(&g_work_lock);
    if (!g_work_tail)
    {
        g_work_head = g_work_tail = item;
    }
    else
    {
        g_work_tail->next = item;
        g_work_tail = item;
    }
    g_num_queued++;
    pthread_cond_signal(&g_work_cond);
    pthread_mutex_unlock(&g_work_lock);
}

/******************************************************************************
 * Pop one socket off the shared queue. Blocks if queue is empty.
 * Returns -1 if shutting down.
 *****************************************************************************/
static int get_work(void)
{
    pthread_mutex_lock(&g_work_lock);

    while (!g_shutdown_flag && (g_work_head == NULL))
    {
        pthread_cond_wait(&g_work_cond, &g_work_lock);
    }
    if (g_shutdown_flag)
    {
        pthread_mutex_unlock(&g_work_lock);
        return -1; // signal to workers to exit
    }
    // remove head
    WorkItem *item = g_work_head;
    g_work_head = item->next;
    if (!g_work_head)
        g_work_tail = NULL;

    g_num_queued--;
    int fd = item->client_fd;
    free(item);

    pthread_mutex_unlock(&g_work_lock);
    return fd;
}

/******************************************************************************
 * Utility: find existing key or return -1 if not found.
 *****************************************************************************/
static int find_key_index(const char *keyname)
{
    for (int i = 0; i < MAX_KEYS; i++)
    {
        if (g_keys[i].state != STATE_INVALID &&
            strncmp(g_keys[i].name, keyname, 31) == 0)
        {
            return i;
        }
    }
    return -1;
}

/******************************************************************************
 * Utility: find a free or invalid slot or return -1 if table full.
 *****************************************************************************/
static int find_free_index(void)
{
    for (int i = 0; i < MAX_KEYS; i++)
    {
        if (g_keys[i].state == STATE_INVALID)
        {
            return i;
        }
    }
    return -1;
}

/******************************************************************************
 * Helper to mark a request as a failure response.
 * increments 'fail' count, sets response->op_status = 'X'
 *****************************************************************************/
static void mark_failure(struct request *resp)
{
    pthread_mutex_lock(&g_stats_lock);
    g_num_fail++;
    pthread_mutex_unlock(&g_stats_lock);

    resp->op_status = 'X';
}

/******************************************************************************
 * Write operation:
 * 1) possibly find existing key or get a new slot.
 * 2) mark it BUSY; do the random usleep
 * 3) write data to /tmp/data.<slot>
 *****************************************************************************/
static void handle_write(int client_fd, struct request *req, int data_len)
{
    // Update stats
    pthread_mutex_lock(&g_stats_lock);
    g_num_write++;
    pthread_mutex_unlock(&g_stats_lock);

    // We'll read 'data_len' bytes from the socket.
    char *buf = malloc(data_len);
    if (!buf)
    {
        mark_failure(req);
        write(client_fd, req, sizeof(*req));
        return;
    }

    // read data from the client
    int total_read = 0;
    while (total_read < data_len)
    {
        int r = read(client_fd, buf + total_read, data_len - total_read);
        if (r <= 0)
        {
            // read error => fail
            mark_failure(req);
            write(client_fd, req, sizeof(*req));
            free(buf);
            return;
        }
        total_read += r;
    }

    // At this point, we have the data in 'buf'.
    // Next: find or allocate a slot for the key
    pthread_mutex_lock(&g_keys_lock);

    // Check if key already exists
    int idx = find_key_index(req->name);
    if (idx < 0)
    {
        // not found => find free slot
        idx = find_free_index();
        if (idx < 0)
        {
            // table is full => fail
            pthread_mutex_unlock(&g_keys_lock);
            mark_failure(req);
            write(client_fd, req, sizeof(*req));
            free(buf);
            return;
        }
        // brand-new key
        strncpy(g_keys[idx].name, req->name, 31);
        g_keys[idx].name[31] = '\0';
        g_keys[idx].state = STATE_BUSY;

        // increment the object count
        pthread_mutex_lock(&g_stats_lock);
        g_num_objects++;
        pthread_mutex_unlock(&g_stats_lock);
    }
    else
    {
        // if found in VALID or BUSY state => check
        if (g_keys[idx].state == STATE_BUSY)
        {
            // can't write to busy => fail
            pthread_mutex_unlock(&g_keys_lock);
            mark_failure(req);
            write(client_fd, req, sizeof(*req));
            free(buf);
            return;
        }
        // Otherwise, it's a rewrite of a VALID key => put it to BUSY
        g_keys[idx].state = STATE_BUSY;
    }

    pthread_mutex_unlock(&g_keys_lock);

    // Sleep for concurrency stress, per project instructions
    srandom(time(NULL) ^ pthread_self());
    usleep(random() % 10000);

    // Write data to /tmp/data.<idx>
    char filename[64];
    snprintf(filename, sizeof(filename), "/tmp/data.%d", idx);

    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        // can't open => fail
        pthread_mutex_lock(&g_keys_lock);
        // revert slot if we had created it
        if (g_keys[idx].state == STATE_BUSY)
        {
            g_keys[idx].state = STATE_INVALID;
            g_num_objects--;
        }
        pthread_mutex_unlock(&g_keys_lock);

        mark_failure(req);
        write(client_fd, req, sizeof(*req));
        free(buf);
        return;
    }
    int written = write(fd, buf, data_len);
    close(fd);

    if (written != data_len)
    {
        // incomplete => fail
        pthread_mutex_lock(&g_keys_lock);
        if (g_keys[idx].state == STATE_BUSY)
        {
            g_keys[idx].state = STATE_INVALID;
            g_num_objects--;
        }
        pthread_mutex_unlock(&g_keys_lock);

        mark_failure(req);
        write(client_fd, req, sizeof(*req));
        free(buf);
        return;
    }

    // success => mark state VALID
    pthread_mutex_lock(&g_keys_lock);
    g_keys[idx].state = STATE_VALID;
    pthread_mutex_unlock(&g_keys_lock);

    // respond 'K'
    req->op_status = 'K';
    write(client_fd, req, sizeof(*req));
    free(buf);
}

/******************************************************************************
 * Read operation: read back data from /tmp/data.<slot>, if it exists.
 *****************************************************************************/
static void handle_read(int client_fd, struct request *req)
{
    pthread_mutex_lock(&g_stats_lock);
    g_num_read++;
    pthread_mutex_unlock(&g_stats_lock);

    pthread_mutex_lock(&g_keys_lock);
    int idx = find_key_index(req->name);
    if (idx < 0 || g_keys[idx].state != STATE_VALID)
    {
        // not found or busy => fail
        pthread_mutex_unlock(&g_keys_lock);
        mark_failure(req);
        write(client_fd, req, sizeof(*req));
        return;
    }

    // open the file
    char filename[64];
    snprintf(filename, sizeof(filename), "/tmp/data.%d", idx);

    int fd = open(filename, O_RDONLY);
    pthread_mutex_unlock(&g_keys_lock);

    if (fd < 0)
    {
        // fail
        mark_failure(req);
        write(client_fd, req, sizeof(*req));
        return;
    }

    // read the contents
    char buffer[4096]; // guaranteed max
    int n = read(fd, buffer, sizeof(buffer));
    close(fd);

    if (n < 0)
    {
        // read error
        mark_failure(req);
        write(client_fd, req, sizeof(*req));
        return;
    }

    // success => respond with 'K' and length, then data
    req->op_status = 'K';
    // store length in ASCII decimal
    char len_str[8];
    snprintf(len_str, sizeof(len_str), "%d", n);
    memcpy(req->len, len_str, 8);

    write(client_fd, req, sizeof(*req));
    if (n > 0)
    {
        write(client_fd, buffer, n);
    }
}

/******************************************************************************
 * Delete operation
 *****************************************************************************/
static void handle_delete(int client_fd, struct request *req)
{
    pthread_mutex_lock(&g_stats_lock);
    g_num_delete++;
    pthread_mutex_unlock(&g_stats_lock);

    pthread_mutex_lock(&g_keys_lock);
    int idx = find_key_index(req->name);
    if (idx < 0 || g_keys[idx].state != STATE_VALID)
    {
        pthread_mutex_unlock(&g_keys_lock);
        mark_failure(req);
        write(client_fd, req, sizeof(*req));
        return;
    }
    // mark as invalid
    g_keys[idx].state = STATE_INVALID;
    g_num_objects--;
    pthread_mutex_unlock(&g_keys_lock);

    // remove on-disk file if needed
    char filename[64];
    snprintf(filename, sizeof(filename), "/tmp/data.%d", idx);
    unlink(filename);

    req->op_status = 'K';
    write(client_fd, req, sizeof(*req));
}

/******************************************************************************
 * Handle one client request on socket 'fd'. Read request, parse it,
 * perform the operation, and send a response. Then close 'fd'.
 *****************************************************************************/
static void handle_client(int fd)
{
    struct request req;
    memset(&req, 0, sizeof(req));

    int r = read(fd, &req, sizeof(req));
    if (r <= 0)
    {
        // no data => just close
        close(fd);
        return;
    }
    // parse
    int length = atoi(req.len);
    char op = req.op_status;

    // If it's a Quit command from the client, we can gracefully set the global
    // shutdown flag. (Some instructions prefer "quit" only from console, but
    // here's an example of handling 'Q' from the network side.)
    if (op == 'Q')
    {
        // Mark success back to the client.
        req.op_status = 'K';
        write(fd, &req, sizeof(req));
        close(fd);

        // Set the shutdown flag and wake up any waiting workers.
        pthread_mutex_lock(&g_work_lock);
        g_shutdown_flag = 1;
        pthread_cond_broadcast(&g_work_cond);
        pthread_mutex_unlock(&g_work_lock);
        for (int i = 0; i < 4; i++)
            queue_work(-1);
        // Close the global listening socket if it's open.
        if (g_listen_fd != -1)
        {
            close(g_listen_fd);
            g_listen_fd = -1;
        }
        return;
    }

    // W, R, D
    switch (op)
    {
    case 'W':
        handle_write(fd, &req, length);
        break;
    case 'R':
        handle_read(fd, &req);
        break;
    case 'D':
        handle_delete(fd, &req);
        break;
    default:
        // unknown => fail
        mark_failure(&req);
        write(fd, &req, sizeof(req));
        break;
    }
    close(fd);
}

/******************************************************************************
 * Worker thread main loop
 *****************************************************************************/
static void *worker_thread(void *arg)
{
    (void)arg; // unused
    while (!g_shutdown_flag)
    {
        int fd = get_work();
        if (fd < 0)
        {
            // means shutdown
            break;
        }
        handle_client(fd);
    }
    return NULL;
}

/******************************************************************************
 * Listener thread: accept new connections and queue them
 *****************************************************************************/
static void *listener_thread(void *arg)
{
    int listen_fd = *((int *)arg);
    free(arg);

    while (!g_shutdown_flag)
    {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);

        int client_fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (client_fd < 0)
        {
            // accept() fails if we have closed listen_fd in main
            if (g_shutdown_flag)
            {
                // This means we intentionally closed it during shutdown
                break;
            }
            perror("accept error");
            break;
        }
        // We have a new connection, enqueue it
        queue_work(client_fd);
    }
    // Cleanup, then exit the thread
    return NULL;
}
/******************************************************************************
 * Print stats: # objects, # read, # write, # delete, # fail, # queued
 *****************************************************************************/
static void print_stats(void)
{
    int nobj, nr, nw, nd, nf, nq;

    // Lock g_stats_lock to read the stats it protects
    pthread_mutex_lock(&g_stats_lock);
    nobj = g_num_objects;
    nr = g_num_read;
    nw = g_num_write;
    nd = g_num_delete;
    nf = g_num_fail;
    pthread_mutex_unlock(&g_stats_lock);

    // Lock g_work_lock to read g_num_queued
    pthread_mutex_lock(&g_work_lock);
    nq = g_num_queued;
    pthread_mutex_unlock(&g_work_lock);

    printf("STATS:\n");
    printf("  objects:   %d\n", nobj);
    printf("  reads:     %d\n", nr);
    printf("  writes:    %d\n", nw);
    printf("  deletes:   %d\n", nd);
    printf("  failures:  %d\n", nf);
    printf("  queued:    %d\n", nq);
}

/******************************************************************************
 * Main thread: parse optional port argument, create worker threads, start
 * the listener thread, then wait for console input: "stats" or "quit".
 *****************************************************************************/
int main(int argc, char *argv[])
{
    // Remove old data.* files (optional, but often helpful)
    system("rm -f /tmp/data.*");

    int port = 5000;
    if (argc > 1)
    {
        port = atoi(argv[1]);
        if (port <= 0)
        {
            port = 5000;
        }
    }

    // Initialize the table
    for (int i = 0; i < MAX_KEYS; i++)
    {
        g_keys[i].state = STATE_INVALID;
        g_keys[i].name[0] = '\0';
    }

    // Create listening socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("socket");
        exit(1);
    }
    g_listen_fd = sock;
    // allow reuse if in TIME_WAIT
    int optval = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(sock);
        exit(1);
    }
    if (listen(sock, 8) < 0)
    {
        perror("listen");
        close(sock);
        exit(1);
    }

    printf("dbserver: listening on port %d ...\n", port);

    // Start 4 worker threads
    pthread_t workers[4];
    for (int i = 0; i < 4; i++)
    {
        pthread_create(&workers[i], NULL, worker_thread, NULL);
    }

    // Start the listener thread
    pthread_t lstnr;
    int *sock_ptr = malloc(sizeof(int));
    *sock_ptr = sock;
    pthread_create(&lstnr, NULL, listener_thread, sock_ptr);

    // Main thread: read from stdin for "stats" / "quit"
    char line[128];
    while (1)
    {
        if (fgets(line, sizeof(line), stdin) == NULL)
        {
            // Instead of quitting immediately on EOF, wait a moment and continue.
            sleep(1);
            // Also, if shutdown was requested from a network "quit", break.
            if (g_shutdown_flag)
                break;
            continue;
        }
        if (g_shutdown_flag)
            break;

        if (strncmp(line, "stats", 5) == 0)
        {
            print_stats();
        }
        else if (strncmp(line, "quit", 4) == 0)
        {
            g_shutdown_flag = 1;
            break;
        }
        else
        {
            printf("commands: stats | quit\n");
        }
    }

    // Tell everyone to shut down
    pthread_mutex_lock(&g_work_lock);
    g_shutdown_flag = 1;

    /*
     * Force the listener thread's accept() call to fail
     * by closing the socket here.
     */
    close(sock);

    /*
     * Signal any worker threads waiting for new work.
     * This is helpful if your workers can block on the queue.
     */
    pthread_cond_broadcast(&g_work_cond);
    pthread_mutex_unlock(&g_work_lock);

    /*
     * Now join the listener thread,
     * which should exit promptly because accept() fails.
     */
    pthread_join(lstnr, NULL);

    /* Join the worker threads */
    for (int i = 0; i < 4; i++)
    {
        pthread_join(workers[i], NULL);
    }

    printf("dbserver: shutting down.\n");
    return 0;
}
