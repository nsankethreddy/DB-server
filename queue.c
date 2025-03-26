#include <stdio.h>
#include <stdlib.h>
#include "queue.h"

void enqueue(queue_t *queue, void *element)
{
        if (queue == NULL)
        {
                fprintf(stderr, "Queue is not initialized.\n");
                exit(EXIT_FAILURE);
        }

        queue_t *temp = queue;
        while (temp->next != NULL)
        {
                temp = temp->next;
        }

        temp->next = (queue_t *)malloc(sizeof(queue_t));
        if (temp->next == NULL)
        {
                perror("Failed to allocate memory for new node");
                exit(EXIT_FAILURE);
        }

        temp->next->element = element;
        temp->next->next = NULL;
}

void *dequeue(queue_t *queue)
{
        if (queue == NULL)
        {
                fprintf(stderr, "Queue is not initialized.\n");
                exit(EXIT_FAILURE);
        }

        if (queue->next == NULL)
        {
                return NULL;
        }

        queue_t *temp = queue->next;
        void *element = temp->element;
        queue->next = temp->next;
        free(temp);

        return element;
}

void print_queue(queue_t *queue)
{
        if (queue == NULL)
        {
                printf("Queue is not initialized.\n");
                return;
        }

        queue_t *temp = queue->next;
        if (temp == NULL)
        {
                printf("[Empty]");
                return;
        }

        while (temp != NULL)
        {
                process_t *proc = (process_t *)temp->element;
                printf("[id: %d, name: %s]", proc->id, proc->name);
                if (temp->next != NULL)
                {
                        printf(" -> ");
                }
                temp = temp->next;
        }
}
