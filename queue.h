#ifndef QUEUE_H
#define QUEUE_H

struct queue_t;

typedef struct queue_t
{
        void *element;
        struct queue_t *next;
} queue_t;

typedef struct process_t
{
        int id;
        char *name;
} process_t;

void enqueue(queue_t *queue, void *element);
void *dequeue(queue_t *queue);
void print_queue(queue_t *queue);

#endif
