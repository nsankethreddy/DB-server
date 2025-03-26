#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include "proj2.h"
#include "queue.h"

typedef enum
{
    STATE_INVALID,
    STATE_BUSY,
    STATE_VALID
} State;

typedef struct
{
    char name[32];
    State state;
} KeyRecord;

#define MAX_KEYS 200
static KeyRecord g_keys[MAX_KEYS];
static pthread_mutex_t g_keys_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct
{
    int num_objects;
    int num_write;
    int num_read;
    int num_delete;
    int num_fail;
    int num_queued;
} Stats;
static Stats g_stats = {0};
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_shutdown_flag = 0;
static int g_listen_fd = -1;

static queue_t *g_work_queue; // Use the provided queue (dummy head)

static pthread_mutex_t g_work_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work_cond = PTHREAD_COND_INITIALIZER;

static void queue_work(int fd)
{
    int *p = malloc(sizeof(int));
    if (!p)
        return;
    *p = fd;
    pthread_mutex_lock(&g_work_lock);
    enqueue(g_work_queue, p);
    g_stats.num_queued++;
    pthread_cond_signal(&g_work_cond);
    pthread_mutex_unlock(&g_work_lock);
}

static int get_work(void)
{
    pthread_mutex_lock(&g_work_lock);
    while (!g_shutdown_flag)
    {
        void *elem = dequeue(g_work_queue);
        if (elem != NULL)
        {
            g_stats.num_queued--;
            pthread_mutex_unlock(&g_work_lock);
            int sock = *((int *)elem);
            free(elem);
            return sock;
        }
        pthread_cond_wait(&g_work_cond, &g_work_lock);
    }
    pthread_mutex_unlock(&g_work_lock);
    return -1;
}

static int find_key_index(const char *keyname)
{
    for (int i = 0; i < MAX_KEYS; i++)
    {
        if (g_keys[i].state != STATE_INVALID && strncmp(g_keys[i].name, keyname, 31) == 0)
            return i;
    }
    return -1;
}

static int find_free_index(void)
{
    for (int i = 0; i < MAX_KEYS; i++)
    {
        if (g_keys[i].state == STATE_INVALID)
            return i;
    }
    return -1;
}

static void mark_failure(struct request *resp)
{
    pthread_mutex_lock(&g_stats_lock);
    g_stats.num_fail++;
    pthread_mutex_unlock(&g_stats_lock);
    resp->op_status = 'X';
}

static void handle_write(int client_fd, struct request *req, int data_len)
{
    char *buf = malloc(data_len);
    if (!buf)
    {
        mark_failure(req);
        write(client_fd, req, sizeof(*req));
        return;
    }
    int total_read = 0;
    while (total_read < data_len)
    {
        int r = read(client_fd, buf + total_read, data_len - total_read);
        if (r <= 0)
        {
            mark_failure(req);
            write(client_fd, req, sizeof(*req));
            free(buf);
            return;
        }
        total_read += r;
    }
    pthread_mutex_lock(&g_keys_lock);
    int idx = find_key_index(req->name);
    if (idx < 0)
    {
        idx = find_free_index();
        if (idx < 0)
        {
            pthread_mutex_unlock(&g_keys_lock);
            mark_failure(req);
            write(client_fd, req, sizeof(*req));
            free(buf);
            return;
        }
        strncpy(g_keys[idx].name, req->name, 31);
        g_keys[idx].name[31] = '\0';
        g_keys[idx].state = STATE_BUSY;
        pthread_mutex_lock(&g_stats_lock);
        g_stats.num_objects++;
        pthread_mutex_unlock(&g_stats_lock);
    }
    else
    {
        if (g_keys[idx].state == STATE_BUSY)
        {
            pthread_mutex_unlock(&g_keys_lock);
            mark_failure(req);
            write(client_fd, req, sizeof(*req));
            free(buf);
            return;
        }
        g_keys[idx].state = STATE_BUSY;
    }
    pthread_mutex_unlock(&g_keys_lock);
    srandom(time(NULL) ^ pthread_self());
    usleep(random() % 10000);
    char filename[64];
    snprintf(filename, sizeof(filename), "/tmp/data.%d", idx);
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        pthread_mutex_lock(&g_keys_lock);
        if (g_keys[idx].state == STATE_BUSY)
        {
            g_keys[idx].state = STATE_INVALID;
            pthread_mutex_lock(&g_stats_lock);
            g_stats.num_objects--;
            pthread_mutex_unlock(&g_stats_lock);
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
        pthread_mutex_lock(&g_keys_lock);
        if (g_keys[idx].state == STATE_BUSY)
        {
            g_keys[idx].state = STATE_INVALID;
            pthread_mutex_lock(&g_stats_lock);
            g_stats.num_objects--;
            pthread_mutex_unlock(&g_stats_lock);
        }
        pthread_mutex_unlock(&g_keys_lock);
        mark_failure(req);
        write(client_fd, req, sizeof(*req));
        free(buf);
        return;
    }
    pthread_mutex_lock(&g_keys_lock);
    g_keys[idx].state = STATE_VALID;
    pthread_mutex_unlock(&g_keys_lock);
    req->op_status = 'K';
    write(client_fd, req, sizeof(*req));
    free(buf);
}

static void handle_read(int client_fd, struct request *req)
{
    pthread_mutex_lock(&g_keys_lock);
    int idx = find_key_index(req->name);
    if (idx < 0 || g_keys[idx].state != STATE_VALID)
    {
        pthread_mutex_unlock(&g_keys_lock);
        mark_failure(req);
        write(client_fd, req, sizeof(*req));
        return;
    }
    char filename[64];
    snprintf(filename, sizeof(filename), "/tmp/data.%d", idx);
    int fd = open(filename, O_RDONLY);
    pthread_mutex_unlock(&g_keys_lock);
    if (fd < 0)
    {
        mark_failure(req);
        write(client_fd, req, sizeof(*req));
        return;
    }
    char buffer[4096];
    memset(buffer, 0, sizeof(buffer));
    int n = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (n < 0)
    {
        mark_failure(req);
        write(client_fd, req, sizeof(*req));
        return;
    }

    // Build a new response struct and ensure it is fully initialized.
    struct request resp;
    memset(&resp, 0, sizeof(resp)); // Fully zero the structure
    resp.op_status = 'K';
    // Copy the name field, if desired (or leave it as is)
    strncpy(resp.name, req->name, sizeof(resp.name) - 1);
    // Set the length field properly:
    snprintf(resp.len, sizeof(resp.len), "%d", n);

    write(client_fd, &resp, sizeof(resp));
    if (n > 0)
        write(client_fd, buffer, n);
}

static void handle_delete(int client_fd, struct request *req)
{
    pthread_mutex_lock(&g_keys_lock);
    int idx = find_key_index(req->name);
    if (idx < 0 || g_keys[idx].state != STATE_VALID)
    {
        pthread_mutex_unlock(&g_keys_lock);
        mark_failure(req);
        write(client_fd, req, sizeof(*req));
        return;
    }
    g_keys[idx].state = STATE_INVALID;
    pthread_mutex_lock(&g_stats_lock);
    g_stats.num_objects--;
    pthread_mutex_unlock(&g_stats_lock);
    pthread_mutex_unlock(&g_keys_lock);
    char filename[64];
    snprintf(filename, sizeof(filename), "/tmp/data.%d", idx);
    unlink(filename);
    req->op_status = 'K';
    write(client_fd, req, sizeof(*req));
}

static void process_request(int client_fd, struct request *req)
{
    int length = atoi(req->len);
    char op = req->op_status;
    if (op == 'W')
    {
        pthread_mutex_lock(&g_stats_lock);
        g_stats.num_write++;
        pthread_mutex_unlock(&g_stats_lock);
        handle_write(client_fd, req, length);
    }
    else if (op == 'R')
    {
        pthread_mutex_lock(&g_stats_lock);
        g_stats.num_read++;
        pthread_mutex_unlock(&g_stats_lock);
        handle_read(client_fd, req);
    }
    else if (op == 'D')
    {
        pthread_mutex_lock(&g_stats_lock);
        g_stats.num_delete++;
        pthread_mutex_unlock(&g_stats_lock);
        handle_delete(client_fd, req);
    }
    else if (op == 'Q')
    {
        req->op_status = 'K';
        write(client_fd, req, sizeof(*req));
        close(client_fd);
        pthread_mutex_lock(&g_work_lock);
        g_shutdown_flag = 1;
        pthread_cond_broadcast(&g_work_cond);
        pthread_mutex_unlock(&g_work_lock);
        if (g_listen_fd != -1)
        {
            shutdown(g_listen_fd, SHUT_RDWR);
            close(g_listen_fd);
            g_listen_fd = -1;
        }
    }
    else
    {
        mark_failure(req);
        write(client_fd, req, sizeof(*req));
        close(client_fd);
    }
}

static void handle_client(int fd)
{
    struct request req;
    memset(&req, 0, sizeof(req));
    int r = read(fd, &req, sizeof(req));
    if (r <= 0)
    {
        close(fd);
        return;
    }
    process_request(fd, &req);
    close(fd);
}

static void *worker_thread(void *arg)
{
    (void)arg;
    while (!g_shutdown_flag)
    {
        int fd = get_work();
        if (fd < 0)
            break;
        handle_client(fd);
    }
    return NULL;
}

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
            if (g_shutdown_flag)
                break;
            continue;
        }
        queue_work(client_fd);
    }
    return NULL;
}

static void print_stats(void)
{
    int nobj, nr, nw, nd, nf, nq;
    pthread_mutex_lock(&g_stats_lock);
    nobj = g_stats.num_objects;
    nr = g_stats.num_read;
    nw = g_stats.num_write;
    nd = g_stats.num_delete;
    nf = g_stats.num_fail;
    pthread_mutex_unlock(&g_stats_lock);
    pthread_mutex_lock(&g_work_lock);
    nq = g_stats.num_queued;
    pthread_mutex_unlock(&g_work_lock);
    printf("STATS:\n");
    printf("  objects:   %d\n", nobj);
    printf("  reads:     %d\n", nr);
    printf("  writes:    %d\n", nw);
    printf("  deletes:   %d\n", nd);
    printf("  failures:  %d\n", nf);
    printf("  queued:    %d\n", nq);
}

int main(int argc, char *argv[])
{
    system("rm -f /tmp/data.*");
    int port = 5000;
    if (argc > 1)
    {
        port = atoi(argv[1]);
        if (port <= 0)
            port = 5000;
    }
    for (int i = 0; i < MAX_KEYS; i++)
    {
        g_keys[i].state = STATE_INVALID;
        g_keys[i].name[0] = '\0';
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        exit(1);
    g_listen_fd = sock;
    int optval = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        exit(1);
    }
    if (listen(sock, 8) < 0)
    {
        close(sock);
        exit(1);
    }
    printf("dbserver: listening on port %d ...\n", port);
    g_work_queue = malloc(sizeof(queue_t));
    if (!g_work_queue)
    {
        fprintf(stderr, "Failed to allocate work queue\n");
        exit(1);
    }
    g_work_queue->element = NULL;
    g_work_queue->next = NULL;
    pthread_t workers[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&workers[i], NULL, worker_thread, NULL);
    pthread_t lstnr;
    int *sock_ptr = malloc(sizeof(int));
    *sock_ptr = sock;
    pthread_create(&lstnr, NULL, listener_thread, sock_ptr);
    char line[128];
    while (!g_shutdown_flag && fgets(line, sizeof(line), stdin))
    {
        if (strncmp(line, "stats", 5) == 0)
        {
            print_stats();
        }
        else if (strncmp(line, "quit", 4) == 0)
        {
            pthread_mutex_lock(&g_work_lock);
            g_shutdown_flag = 1;
            pthread_cond_broadcast(&g_work_cond);
            pthread_mutex_unlock(&g_work_lock);
            if (g_listen_fd != -1)
            {
                shutdown(g_listen_fd, SHUT_RDWR);
                close(g_listen_fd);
                g_listen_fd = -1;
            }
            break;
        }
        else
        {
            printf("commands: stats | quit\n");
        }
    }
    pthread_mutex_lock(&g_work_lock);
    g_shutdown_flag = 1;
    if (g_listen_fd != -1)
    {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    pthread_cond_broadcast(&g_work_cond);
    pthread_mutex_unlock(&g_work_lock);
    pthread_join(lstnr, NULL);
    for (int i = 0; i < 4; i++)
        pthread_join(workers[i], NULL);
    // After joining all threads
    free(g_work_queue);
    printf("dbserver: shutting down.\n");
    return 0;
}
