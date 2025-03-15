/*
 * file:        dbtest.c
 * description: tester for Project 2 database
 *
 * needs to clean up.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <zlib.h>
#include <pthread.h>
#include <argp.h>
#include <assert.h>

#include "proj2.h"

/* --------- argument parsing ---------- */

static struct argp_option options[] = {
    {"threads",      't', "NUM",  0, "number of threads"},
    {"count",        'n', "NUM",  0, "number of requests"},
    {"port",         'p', "PORT", 0, "TCP port to connect to (default 5000)"},
    {"set",          'S', "KEY",  0, "set KEY to VALUE"},
    {"get",          'G', "KEY",  0, "get value for KEY"},
    {"delete",       'D', "KEY",  0, "delete KEY"},
    {"quit",         'q',  0,     0, "send QUIT command"},
    {"max",          'm', "NUM",  0, "max number of keys (default 200)"},
    {"test",         'T',  0,     0, "10 simultaneous requests"},
    {"log",          'l', "FILE", 0, "log output to FILE"},
    {"overload",     'O',  0,     0, "try to create >200 keys"},
    {0}
};

enum {OP_SET = 1, OP_GET = 2, OP_DELETE = 3, OP_QUIT = 4};

struct args {
    int nthreads;
    int count;
    int port;
    int max;
    int op;
    int test;
    int overload;
    char *key;
    char *val;
    char *logfile;
    FILE *logfp;
    pthread_mutex_t logm;
    struct sockaddr_in addr;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    struct args *a = state->input;
    switch (key) {
    case ARGP_KEY_INIT:
        a->nthreads = 1;
        a->count = 1000;
        a->port = 5000;
        a->max = 200;
        a->logfp = NULL;
        pthread_mutex_init(&a->logm, NULL);
        break;

    case 'O':
        a->overload = 1;
        break;
        
    case 'l':
        a->logfile = arg;
        if ((a->logfp = fopen(arg, "w")) == NULL)
            fprintf(stderr, "Error opening logfile : %s : %s\n", arg,
                    strerror(errno)), exit(1);
        break;
        
    case 'T':
        a->test = 1;
        break;
        
    case 'q':
        a->op = OP_QUIT;
        break;
        
    case 'm':
        a->max = atoi(arg);
        break;
        
    case 'G':
        a->op = OP_GET;
        if (strlen(arg) > 30)
            printf("key must be <= 30 chars\n"), argp_usage(state);
        a->key = arg;
        break;
        
    case 'S':
        a->op = OP_SET;
        if (strlen(arg) > 30)
            printf("key must be <= 30 chars\n"), argp_usage(state);
        a->key = arg;
        break;

    case 'D':
        a->op = OP_DELETE;
        if (strlen(arg) > 30)
            printf("key must be <= 30 chars\n"), argp_usage(state);
        a->key = arg;
        break;
        
    case 't':
        a->nthreads = atoi(arg); break;

    case 'n':
        a->count = atoi(arg); break;

    case 'p':
        a->port = atoi(arg);
        break;
        
    case ARGP_KEY_ARG:
        if (state->arg_num == 0 && a->op == OP_SET)
            a->val = arg;
        else
            argp_usage(state);
        break;
    }
    return 0;
}

static struct argp argp = { options, parse_opt, NULL, NULL};
        
/* --------- everything else ---------- */

/* keep track of the requests we've sent
 */
struct {
    char name[32];
    int len;
    int crc;
    int busy;
} table[150];
int n_objects;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

/* if #objects is small, write new ones
 * if medium (30-130), write/read
 * if large (>130), read/delete
 */
char get_op(struct args *args)
{
    int hi = args->max * 85 / 100,
        lo = args->max * 15 / 100;
    char op = 'W';
    int n = random();
    pthread_mutex_lock(&m);
    int nn = n_objects;
    pthread_mutex_unlock(&m);
    
    if (nn >= hi)
        op = (n % 20 > 10) ? 'R' : 'D';
    if (nn >= lo)
        op = ((random() % 100) > (nn - lo)) ? 'W' : 'R';
    return op;
}

int pick_random(void)
{
    pthread_mutex_lock(&m);
    int num;
    for (int i = 0; i < 20; i++) {
        num = random() % 150;
        if (table[num].len > 0 && !table[num].busy) {
            table[num].busy = 1;
            pthread_mutex_unlock(&m);
            return num;
        }
    }
    pthread_mutex_unlock(&m);
    return -1;
}

int get_free(void)
{
    int num;
    pthread_mutex_lock(&m);
    for (num = 0; num < 150; num++)
        if (table[num].len == 0 && !table[num].busy)
            break;
    table[num].busy = 1;
    pthread_mutex_unlock(&m);

    return num;
}

void randstr(char *buf, int len)
{
    for (int i = 0; i < len; i++)
        buf[i] = 'A' + (random() % 25);
}

int do_connect(struct sockaddr_in *addr)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0 || connect(sock, (struct sockaddr*)addr, sizeof(*addr)) < 0)
        fprintf(stderr, "can't connect: %s\n", strerror(errno)), exit(0);
    return sock;
}

void *thread(void *_ptr)
{
    struct args *a = _ptr;
    struct sockaddr_in *addr = &a->addr;
    struct request rq;
    int val, num, saved_crc, saved_len;
    char buf[4096];

    for (int i = 0; i < a->count / a->nthreads; i++) {
        int sock = do_connect(addr);
        char op = get_op(a);
        char name[32];

        memset(&rq, 0, sizeof(rq));
        
        if (op == 'W') {
            int num = -1;
            if (random() % 10 < 2)
                num = pick_random();
            int rewrite = (num != -1);

            if (rewrite) {
                assert(table[num].busy);
                pthread_mutex_lock(&m);
                strcpy(name, table[num].name);
                pthread_mutex_unlock(&m);
            }
            else {
                pthread_mutex_lock(&m);
                n_objects++;
                pthread_mutex_unlock(&m);
                memset(name, 0, sizeof(name));
                randstr(name, 16);
                num = get_free(); /* sets busy=1 */
                assert(table[num].busy);
                strcpy(table[num].name, name);
            }
            
            /* invariant: name == table[num].name, busy=1 
             */
            int len = 20 + random() % 600;
            randstr(buf, len);
            int _crc = crc32(-1, (unsigned char*)buf, len);

            if (a->logfp) {
                pthread_mutex_lock(&a->logm);
                fprintf(a->logfp, "W %s = %d,%d\n", name, len, _crc);
                pthread_mutex_unlock(&a->logm);
            }
                
            rq.op_status = op;
            sprintf(rq.len, "%d", len);
            sprintf(rq.name, "%s", name);
            write(sock, &rq, sizeof(rq));
            write(sock, buf, len);
            val = read(sock, &rq, sizeof(rq));

            if (val < 0)
                printf("WRITE: REPLY: READ ERROR: %s\n", strerror(errno));
            else if (val < sizeof(rq))
                printf("WRITE: REPLY: SHORT READ: %d\n", val);

            pthread_mutex_lock(&m);
            assert(strcmp(table[num].name, name) == 0);
            strcpy(table[num].name, name);
            table[num].len = len;
            table[num].crc = _crc;
            table[num].busy = 0; 
            pthread_mutex_unlock(&m); /* make helgrind happy */
        }
        else if (op == 'R' || op == 'D') {
            for (num = pick_random(); num == -1; ) {
                usleep(100);
                num = pick_random();
            }
            
            pthread_mutex_lock(&m);
            strcpy(name, table[num].name);
            saved_crc = table[num].crc;
            saved_len = table[num].len;
            pthread_mutex_unlock(&m); /* make helgrind happy */
            
            rq.op_status = op;
            sprintf(rq.len, "0");
            sprintf(rq.name, "%s", name);
            write(sock, &rq, sizeof(rq));

            val = read(sock, &rq, sizeof(rq));
            if (val < 0)
                printf("%c HDR: REPLY: READ ERROR: %s\n", op, strerror(errno));
            else if (val < sizeof(rq)) 
               printf("%c HDR: REPLY: SHORT READ: %d\n", op, val);

            if (op == 'R') {
                int len = atol(rq.len);
                for (void *ptr = buf, *max = ptr+len; ptr < max;) {
                    int n = read(sock, ptr, max-ptr);
                    if (n < 0) {
                        printf("READ DATA: READ ERROR: %s\n", strerror(errno));
                        break;
                    }
                    ptr += n;
                }
                
                int _crc = crc32(-1, (unsigned char*)buf, len);

                if (a->logfp) {
                    pthread_mutex_lock(&a->logm);
                    fprintf(a->logfp, "R %s = %d,%d %d\n\n", name,
                            len, _crc, saved_len);
                    pthread_mutex_unlock(&a->logm);
                }
                    
                if (len != saved_len)
                    printf("READ %s: bad len %d (should be %d)\n",
                           name, len, saved_len);
                if (_crc != saved_crc)
                    printf("READ %s: bad cksum %d (should be %d)\n",
                           name, _crc, saved_crc);

                pthread_mutex_lock(&m);
                table[num].busy = 0;
                pthread_mutex_unlock(&m);
            }
            else if (op == 'D') {
                pthread_mutex_lock(&m);
                table[num].len = 0;
                table[num].busy = 0;
                n_objects--;
                pthread_mutex_unlock(&m);
            }
        }
        /* try some bad accesses here */
        
        close(sock);
    }
    return NULL;
}

void do_del(struct args *args, char *name, char *result, int quiet)
{
    int sock = do_connect(&args->addr);
    
    struct request rq;
    snprintf(rq.name, sizeof(rq.name), "%s", name);
    
    rq.op_status = 'D';
    int val = write(sock, &rq, sizeof(rq));
    if ((val = read(sock, &rq, sizeof(rq))) < 0)
        printf("DEL: REPLY: READ ERROR: %s\n", strerror(errno));
    else if (val < sizeof(rq))
        printf("DEL: REPLY: SHORT READ: %d\n", val);
    else if (rq.op_status != 'K' && !quiet)
        printf("DEL: FAILED (%c)\n", rq.op_status);
    else if (!quiet)
        printf("ok\n");

    if (result != NULL)
        *result = rq.op_status;

    close(sock);
}

void do_set(struct args *args, char *name, void *data, int len, char *result, int quiet)
{
    int sock = do_connect(&args->addr);
    
    struct request rq;
    snprintf(rq.name, sizeof(rq.name), "%s", name);
    int val;
    
    rq.op_status = 'W';
    sprintf(rq.len, "%d", len);
    write(sock, &rq, sizeof(rq));
    write(sock, data, len);
    if ((val = read(sock, &rq, sizeof(rq))) < 0)
        printf("WRITE: REPLY: READ ERROR: %s\n", strerror(errno));
    else if (val < sizeof(rq))
        printf("WRITE: REPLY: SHORT READ: %d\n", val);
    else if (rq.op_status != 'K' && !quiet)
        printf("WRITE: FAILED (%c)\n", rq.op_status);
    else if (!quiet)
        printf("ok\n");

    if (result != NULL)
        *result = rq.op_status;
    close(sock);
}

void do_quit(struct args *args)
{
    int sock = do_connect(&args->addr);
    struct request rq;
    rq.op_status = 'Q';
    write(sock, &rq, sizeof(rq));
    /* if both sides close-on-exit you won't get TIME_WAIT */
}

void do_get(struct args *args, char *name, void *data, int *len_p, char *result)
{
    int val, sock = do_connect(&args->addr);
    struct request rq;
    snprintf(rq.name, sizeof(rq.name), "%s", name);
    
    rq.op_status = 'R';
    sprintf(rq.len, "%d", 0);
    write(sock, &rq, sizeof(rq));
    if ((val = read(sock, &rq, sizeof(rq))) < 0)
        printf("READ: REPLY: READ ERROR: %s\n", strerror(errno));
    else if (val < sizeof(rq))
        printf("READ: REPLY: SHORT READ: %d\n", val);
    else if (rq.op_status != 'K')
        printf("READ: FAILED (%c)\n", rq.op_status);
    else {
        int len = atoi(rq.len);
        char buf[len];

        for (void *ptr = buf, *max = ptr+len; ptr < max; ) {
            int n = read(sock, ptr, max-ptr);
            if (n < 0) {
                printf("READ DATA: READ ERROR: %s\n", strerror(errno));
                break;
            }
            ptr += n;
        }
        if (data != NULL) {
            memcpy(data, buf, len);
            *len_p = len;
        }
        else
            printf("=\"%.*s\"\n", len, buf);
    }

    if (result != NULL)
        *result = rq.op_status;

    close(sock);
}

struct test {
    struct args *a;
    int num;
    char op;
    int delay;
    char name[32];
};

char test_log[2048], *test_p = test_log;

void *test_thread(void *ptr)
{
    struct test *t = ptr;
    struct args *a = t->a;
    char result;
    char data[1024];
    int crc;

    if (t->delay)
        usleep(t->delay);
    
    if (t->op == 'W') {
        int len = 30 + random() % 100;
        randstr(data, len);
        crc = crc32(-1, (unsigned char*)data, len);
        pthread_mutex_lock(&m);
        test_p += sprintf(test_p, "%d: W %s len=%d crc=%x ->\n",
                          t->num, t->name, len, crc);
        pthread_mutex_unlock(&m);
        do_set(a, t->name, data, len, &result, 1);
        pthread_mutex_lock(&m);
        test_p += sprintf(test_p, " - %d: W %s =%c (len=%d crc=%x)\n",
                          t->num, t->name, result, len, crc);
        pthread_mutex_unlock(&m);
    }
    else if (t->op == 'R') {
        int len = 0;
        
        pthread_mutex_lock(&m);
        test_p += sprintf(test_p, "%d: R %s ->\n", t->num, t->name);
        pthread_mutex_unlock(&m);

        do_get(a, t->name, data, &len, &result);
        crc = crc32(-1, (unsigned char*)data, len);

        pthread_mutex_lock(&m);
        test_p += sprintf(test_p, " - %d: R %s =%c (len %d crc=%x)\n",
                          t->num, t->name, result, len, crc);
        pthread_mutex_unlock(&m);
    }
    else if (t->op == 'D') {
        pthread_mutex_lock(&m);
        test_p += sprintf(test_p, "%d: D %s ->\n", t->num, t->name);
        pthread_mutex_unlock(&m);

        do_del(a, t->name, &result, 1);

        pthread_mutex_lock(&m);
        test_p += sprintf(test_p, " - %d: D %s =%c\n",
                          t->num, t->name, result);
        pthread_mutex_unlock(&m);
    }
    return NULL;
}
        
void do_test(struct args *a)
{
    pthread_t th[10];
    struct test t[10];
    int i;

    srandom(time(NULL));
    for (i = 0; i < 10; i++) {
        t[i] = (struct test){.a = a, .num = i};
        sprintf(t[i].name, "key%d", i%2);
        if (i < 4)
            t[i].op = 'W';
        else if (i < 8)
            t[i].op = 'R';
        else
            t[i].op = 'D';
        t[i].delay = random() % 5;
        pthread_create(&th[i], NULL, test_thread, &t[i]);
    }
    void *tmp;
    for (i = 0; i < 10; i++) 
        pthread_join(th[i], &tmp);

    printf("%s", test_log);
}

void do_overload(struct args *a)
{
    char name[30];
    char data[100];
    
    for (int i = 0; i < 250; i++) {
        sprintf(name, "KEY-%04d", i);
        randstr(data, sizeof(data));
        do_set(a, name, data, sizeof(data), NULL, 1);
    }

    for (int i = 0; i < 250; i++) {
        sprintf(name, "KEY-%04d", i);
        randstr(data, sizeof(data));
        do_del(a, name, NULL, 1);
    }
}
    
int main(int argc, char **argv)
{
    struct args args;
    memset(&args, 0, sizeof(args));
    
    argp_parse(&argp, argc, argv, 0, 0, &args);
            
    args.addr = (struct sockaddr_in){
        .sin_family = AF_INET,
        .sin_port = htons(args.port),
        .sin_addr.s_addr = inet_addr("127.0.0.1")}; /* localhost */

    if (args.test)
        do_test(&args);
    else if (args.overload)
        do_overload(&args);
    else if (args.op == OP_SET)
        do_set(&args, args.key, args.val, strlen(args.val), NULL, 0);
    else if (args.op == OP_GET)
        do_get(&args, args.key, NULL, NULL, NULL);
    else if (args.op == OP_DELETE)
        do_del(&args, args.key, NULL, 0);
    else if (args.op == OP_QUIT)
        do_quit(&args);
    else if (args.nthreads == 1)
        thread(&args);
    else {
        pthread_t th[args.nthreads];
        for (int i = 0; i < args.nthreads; i++)
            pthread_create(&th[i], NULL, thread, &args);
        void *tmp;
        for (int i = 0; i < args.nthreads; i++)
            pthread_join(th[i], &tmp); /* will wait forever */
    }
    for (int i = 0; i < 150; i++)
        if (table[i].len > 0)
            do_del(&args, table[i].name, NULL, 1);
}

