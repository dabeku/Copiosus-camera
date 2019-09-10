#ifndef COP_LIST_H
#define COP_LIST_H

#include <stdlib.h>
#include <stdio.h>

typedef struct list_item {
    void* data;
    struct list_item *next;
} list_item;

int list_length(struct list_item* head);
struct list_item* list_get(struct list_item* head, int index);
struct list_item* list_push(struct list_item* head, void* data);
struct list_item* list_delete(struct list_item* head, int loc);

#endif /* COP_LIST_H */