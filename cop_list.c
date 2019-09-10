#include "cop_list.h"

int list_length(struct list_item* head) {
    struct list_item *cur_ptr;
    int count = 0;
    cur_ptr = head;
    while(cur_ptr != NULL) {
        cur_ptr = cur_ptr->next;
        count++;
    }
    return count;
}

struct list_item* list_get(struct list_item* head, int index) {
    struct list_item *cur_ptr;
    int i = 0;
    cur_ptr = head;
    while(i < index) {
        cur_ptr = cur_ptr->next;
        i++;
    }
    return cur_ptr;
}

struct list_item* list_push(struct list_item* head, void* data) {
    struct list_item *temp1, *temp2;

    temp1=(struct list_item *)malloc(sizeof(struct list_item));
    temp1->data=data;

    // Copying the Head location into another node.
    temp2=head;

    if(head == NULL) {
        // If List is empty we create First Node.
        head=temp1;
        head->next=NULL;
    } else {
        // Traverse down to end of the list.
        while(temp2->next != NULL) {
            temp2=temp2->next;
        }
        // Append at the end of the list.
        temp1->next=NULL;
        temp2->next=temp1;
    }

    return head;
}

struct list_item* list_delete(struct list_item* head, int loc) {
    struct list_item *prev_ptr, *cur_ptr;
    int i;

    cur_ptr=head;

    if (loc >= list_length(head) || loc < 0) {
        return NULL;
    } else {
        // If the location is starting of the list
        if (loc == 0) {
            head=cur_ptr->next;
            free(cur_ptr);
        } else {
            for (i=0; i<loc; i++) {
                prev_ptr=cur_ptr;
                cur_ptr=cur_ptr->next;
            }
            prev_ptr->next=cur_ptr->next;
            free(cur_ptr);
        }
    }
    return head;
}