/********************************************************
 * dbserver.c
 *
 * A simple multi-threaded database server for Project 2.
 *
 * Build with:
 *   gcc -Wall -O2 -o dbserver dbserver.c -lpthread
 *
 * Usage:
 *   ./dbserver [port]
 * Then type "stats" or "quit" on stdin to interact.
 *
 * This code is a reference implementation and may need
 * further debugging or tuning based on your environment.
 ********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "proj2.h" // struct request { char op_status; char name[31]; char len[8]; }

#define MAX_KEYS 200
#define STATE_INVALID 0
#define STATE_BUSY 1
#define STATE_VALID 2

/* ---------------------------------------------------
   GLOBALS
   ---------------------------------------------------*/
static int g_stop = 0;      // set to 1 when main thread sees "quit"
static int g_listener_sock; // listener socket FD

// Stats
static int g_num_objects = 0; // how many valid objects in table
static int g_read_count = 0;
static int g_write_count = 0;
static int g_delete_count = 0;
static int g_fail_count = 0;
static int g_queued_count = 0; // how many requests are queued (not yet handled)

// Key table
typedef struct
{
    int state;     // 0=invalid,1=busy,2=valid
    char name[31]; // up to 30 chars + nul
    int length;    // size of data on disk
} db_entry_t;

static db_entry_t g_table[MAX_KEYS];

// Protect database and stats
static pthread_mutex_t g_db_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---------------------------------------------------
   WORK QUEUE
   ---------------------------------------------------*/
typedef struct work_item
{
    int sockfd;
    struct work_item *next;
} work_item_t;

static work_item_t *g_work_head = NULL;
static work_item_t *g_work_tail = NULL;

// Protect the queue
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_queue_cond = PTHREAD_COND_INITIALIZER;

/* ---------------------------------------------------
   FUNCTION DECLARATIONS
   ---------------------------------------------------*/

// Thread main functions
void *listener_thread(void *arg);
void *worker_thread(void *arg);

// Helpers for queue
void queue_work(int sockfd);
int get_work(void);

// Database ops
int find_key_index(const char *key);
int find_free_index(void);
void handle_request(int sockfd);

// Main thread commands
void cmd_stats(void);
void cmd_quit(void);

// File ops
int write_data_file(int index, const char *buf, int len);
int read_data_file(int index, char *buf, int len);

/* ---------------------------------------------------
   MAIN
   ---------------------------------------------------*/
int main(int argc, char *argv[])
{
    // Optional: delete old files
    system("rm -f data.*");

    // Initialize the table
    memset(g_table, 0, sizeof(g_table));

    // Optional: seed random for usleep
    srand((unsigned)time(NULL));

    // Parse optional port from command line
    int port = 5000;
    if (argc > 1)
    {
        port = atoi(argv[1]);
        if (port <= 0)
        {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            exit(1);
        }
    }

    // Create listener thread
    pthread_t tlistener;
    if (pthread_create(&tlistener, NULL, listener_thread, &port) != 0)
    {
        perror("pthread_create listener");
        exit(1);
    }

    // Create 4 worker threads
    pthread_t workers[4];
    for (int i = 0; i < 4; i++)
    {
        if (pthread_create(&workers[i], NULL, worker_thread, NULL) != 0)
        {
            perror("pthread_create worker");
            exit(1);
        }
    }

    // Main thread: read commands from stdin
    char line[128];
    while (!g_stop && fgets(line, sizeof(line), stdin) != NULL)
    {
        // remove trailing newline
        line[strcspn(line, "\n")] = '\0';

        if (strcmp(line, "stats") == 0)
        {
            cmd_stats();
        }
        else if (strcmp(line, "quit") == 0)
        {
            cmd_quit();
        }
        else
        {
            printf("Unknown command: %s\n", line);
            printf("Commands: stats, quit\n");
        }
    }

    // If user typed "quit", join listener
    pthread_join(tlistener, NULL);

    // Workers will exit if g_stop=1 and queue is empty
    // so push a few dummy items with sockfd=-1 to "wake" them
    for (int i = 0; i < 4; i++)
    {
        queue_work(-1);
    }
    for (int i = 0; i < 4; i++)
    {
        pthread_join(workers[i], NULL);
    }

    return 0;
}

/* ---------------------------------------------------
   LISTENER THREAD
   ---------------------------------------------------*/
void *listener_thread(void *arg)
{
    int port = *(int *)arg;

    // Create listening socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("socket");
        exit(1);
    }

    // Allow immediate rebinding if stuck in TIME_WAIT
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
    if (listen(sock, 10) < 0)
    {
        perror("listen");
        close(sock);
        exit(1);
    }

    g_listener_sock = sock;
    printf("Listening on port %d...\n", port);

    // Accept loop
    while (!g_stop)
    {
        int client_fd = accept(sock, NULL, NULL);
        if (client_fd < 0)
        {
            if (errno == EINTR)
            {
                // Interrupted by something, keep going
                continue;
            }
            // If user typed "quit" and we closed the socket,
            // accept might fail with EBADF or similar. So break.
            if (g_stop)
                break;
            perror("accept");
            break;
        }
        // Enqueue this connection for a worker
        queue_work(client_fd);
    }

    // Close listening socket
    close(sock);
    return NULL;
}

/* ---------------------------------------------------
   WORKER THREAD
   ---------------------------------------------------*/
void *worker_thread(void *arg)
{
    (void)arg; // unused

    while (1)
    {
        int sockfd = get_work();
        if (sockfd < 0)
        {
            // This signals we should stop
            break;
        }
        // Handle request
        handle_request(sockfd);
        close(sockfd);
    }
    return NULL;
}

/* ---------------------------------------------------
   QUEUE HELPERS
   ---------------------------------------------------*/
void queue_work(int sockfd)
{
    work_item_t *item = (work_item_t *)malloc(sizeof(work_item_t));
    item->sockfd = sockfd;
    item->next = NULL;

    pthread_mutex_lock(&g_queue_mutex);

    // Insert at tail
    if (!g_work_head)
    {
        g_work_head = item;
        g_work_tail = item;
    }
    else
    {
        g_work_tail->next = item;
        g_work_tail = item;
    }
    // Increase queued count if it's a real sock
    if (sockfd >= 0)
    {
        g_queued_count++;
    }

    pthread_cond_signal(&g_queue_cond);
    pthread_mutex_unlock(&g_queue_mutex);
}

int get_work(void)
{
    pthread_mutex_lock(&g_queue_mutex);

    while (!g_work_head)
    {
        // If stop is requested, we might want to break
        if (g_stop)
        {
            pthread_mutex_unlock(&g_queue_mutex);
            return -1;
        }
        pthread_cond_wait(&g_queue_cond, &g_queue_mutex);
    }

    work_item_t *item = g_work_head;
    g_work_head = item->next;
    if (!g_work_head)
        g_work_tail = NULL;

    int sockfd = item->sockfd;
    if (sockfd >= 0)
    {
        g_queued_count--;
    }
    free(item);

    pthread_mutex_unlock(&g_queue_mutex);
    return sockfd;
}

/* ---------------------------------------------------
   HANDLE REQUEST
   ---------------------------------------------------*/
void handle_request(int sockfd)
{
    struct request req;
    memset(&req, 0, sizeof(req));

    // Read request header
    ssize_t n = read(sockfd, &req, sizeof(req));
    if (n < 0)
    {
        perror("read request");
        pthread_mutex_lock(&g_db_mutex);
        g_fail_count++;
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }
    if (n < (ssize_t)sizeof(req))
    {
        // Short read, fail
        pthread_mutex_lock(&g_db_mutex);
        g_fail_count++;
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    char op = req.op_status; // 'W','R','D','Q'
    int length = atoi(req.len);
    char key[31];
    strncpy(key, req.name, 30);
    key[30] = '\0';

    // If Q, handle quickly
    if (op == 'Q')
    {
        // You can do immediate server shutdown or a response
        // Here we just set g_stop=1 to signal shutdown
        // and do not send any response back for QUIT
        pthread_mutex_lock(&g_db_mutex);
        g_stop = 1;
        pthread_mutex_unlock(&g_db_mutex);
        return;
    }

    // For a Write, read the data
    char *wbuf = NULL;
    if (op == 'W' && length > 0)
    {
        wbuf = (char *)malloc(length);
        ssize_t remaining = length;
        char *ptr = wbuf;
        while (remaining > 0)
        {
            n = read(sockfd, ptr, remaining);
            if (n <= 0)
            {
                // Fail
                free(wbuf);
                pthread_mutex_lock(&g_db_mutex);
                g_fail_count++;
                pthread_mutex_unlock(&g_db_mutex);
                return;
            }
            ptr += n;
            remaining -= n;
        }
    }

    // We'll build a response
    struct request resp;
    memset(&resp, 0, sizeof(resp));
    // We'll set resp.op_status to 'K' on success or 'X' on fail.

    // Lock the database
    pthread_mutex_lock(&g_db_mutex);

    // Find or create index
    int idx = find_key_index(key);
    int was_found = 1;
    if (idx < 0 && op == 'W')
    {
        // need to find a free
        idx = find_free_index();
        was_found = 0;
    }

    // If we can't find or can't create an index
    if (idx < 0)
    {
        resp.op_status = 'X';
        g_fail_count++;
        // stats
        if (op == 'R')
            g_read_count++;
        if (op == 'W')
            g_write_count++;
        if (op == 'D')
            g_delete_count++;
        goto send_response;
    }

    // If the entry is busy => fail
    if (g_table[idx].state == STATE_BUSY && op != 'W')
    {
        // we do not allow read/delete of a "busy" item
        resp.op_status = 'X';
        g_fail_count++;
        if (op == 'R')
            g_read_count++;
        if (op == 'D')
            g_delete_count++;
        goto send_response;
    }

    // Actually handle the operation
    switch (op)
    {
    case 'W':
    {
        g_write_count++;
        // Mark busy
        g_table[idx].state = STATE_BUSY;
        // If it's a newly created entry
        if (!was_found)
        {
            strncpy(g_table[idx].name, key, 30);
            g_table[idx].name[30] = '\0';
            // bump number of objects if it was invalid
            g_num_objects++;
        }

        // Sleep up to 10ms as required
        usleep(random() % 10000);

        // Write the data to file
        int rc = write_data_file(idx, wbuf, length);
        if (rc < 0)
        {
            // fail
            g_fail_count++;
            resp.op_status = 'X';
            // revert changes
            g_table[idx].state = STATE_INVALID;
            g_num_objects--;
        }
        else
        {
            // success
            g_table[idx].length = length;
            g_table[idx].state = STATE_VALID;
            resp.op_status = 'K';
        }
        break;
    }
    case 'R':
    {
        g_read_count++;
        // Must be in VALID state
        if (g_table[idx].state != STATE_VALID)
        {
            resp.op_status = 'X';
            g_fail_count++;
            break;
        }
        resp.op_status = 'K';
        // fill resp.len with file size as string
        sprintf(resp.len, "%d", g_table[idx].length);
        break;
    }
    case 'D':
    {
        g_delete_count++;
        // Must be in VALID state
        if (g_table[idx].state != STATE_VALID)
        {
            resp.op_status = 'X';
            g_fail_count++;
            break;
        }
        // Delete
        g_table[idx].state = STATE_INVALID;
        g_table[idx].length = 0;
        g_num_objects--;
        // Optionally remove the file
        char fname[64];
        snprintf(fname, sizeof(fname), "data.%d", idx);
        unlink(fname);

        resp.op_status = 'K';
        break;
    }
    default:
    {
        // unknown op
        resp.op_status = 'X';
        g_fail_count++;
        break;
    }
    }

send_response:;
    // We'll send the response header now
    // If read was successful, we also send file data
    write(sockfd, &resp, sizeof(resp));

    // If it was a successful read, send the actual data
    if (op == 'R' && resp.op_status == 'K')
    {
        int file_len = g_table[idx].length;
        if (file_len > 0)
        {
            char *data_buf = (char *)malloc(file_len);
            if (read_data_file(idx, data_buf, file_len) == file_len)
            {
                // Write to socket
                write(sockfd, data_buf, file_len);
            }
            free(data_buf);
        }
    }

    pthread_mutex_unlock(&g_db_mutex);

    // free wbuf if allocated
    if (wbuf)
        free(wbuf);
}

/* ---------------------------------------------------
   DATABASE HELPER
   ---------------------------------------------------*/
int find_key_index(const char *key)
{
    for (int i = 0; i < MAX_KEYS; i++)
    {
        if (g_table[i].state != STATE_INVALID &&
            strncmp(g_table[i].name, key, 31) == 0)
        {
            return i;
        }
    }
    return -1;
}

int find_free_index(void)
{
    // find an invalid slot
    for (int i = 0; i < MAX_KEYS; i++)
    {
        if (g_table[i].state == STATE_INVALID)
        {
            return i;
        }
    }
    return -1; // full
}

/* ---------------------------------------------------
   FILE OPS
   ---------------------------------------------------*/
int write_data_file(int index, const char *buf, int len)
{
    char filename[64];
    snprintf(filename, sizeof(filename), "data.%d", index);

    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        return -1;
    }
    int total = 0;
    while (total < len)
    {
        int n = write(fd, buf + total, len - total);
        if (n < 0)
        {
            close(fd);
            return -1;
        }
        total += n;
    }
    close(fd);
    return 0;
}

int read_data_file(int index, char *buf, int len)
{
    char filename[64];
    snprintf(filename, sizeof(filename), "data.%d", index);

    int fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        return -1;
    }
    int total = 0;
    while (total < len)
    {
        int n = read(fd, buf + total, len - total);
        if (n < 0)
        {
            close(fd);
            return -1;
        }
        if (n == 0)
        {
            // EOF
            break;
        }
        total += n;
    }
    close(fd);
    return total;
}

/* ---------------------------------------------------
   MAIN THREAD COMMANDS
   ---------------------------------------------------*/
void cmd_stats(void)
{
    pthread_mutex_lock(&g_db_mutex);
    printf("------- Stats -------\n");
    printf("Objects:    %d\n", g_num_objects);
    printf("Reads:      %d\n", g_read_count);
    printf("Writes:     %d\n", g_write_count);
    printf("Deletes:    %d\n", g_delete_count);
    printf("Failures:   %d\n", g_fail_count);
    printf("Queued:     %d\n", g_queued_count);
    printf("---------------------\n");
    pthread_mutex_unlock(&g_db_mutex);
}

void cmd_quit(void)
{
    printf("Shutting down server...\n");
    pthread_mutex_lock(&g_db_mutex);
    g_stop = 1;
    pthread_mutex_unlock(&g_db_mutex);

    // Close listener so accept() fails
    shutdown(g_listener_sock, SHUT_RDWR);
    close(g_listener_sock);
}
