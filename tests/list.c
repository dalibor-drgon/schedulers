/**
 * @file list.c
 * @author Dalibor Drgon
 * @brief Simple test to check the behaviour and validity of list_* functions.
 * @version 0.1
 * @date 2020-05-14
 * 
 * @copyright Copyright (c) 2020 Dalibor Drgon
 * 
 * This is simple test to check the behaviour and validity of list_* functions.
 * Compile as:
 *  gcc -g3 list.c -D __SCHEDULER_TEST2__
 * and execute the resulting binary few times to ensure it's working ok in this
 * context.
 */

#ifdef __SCHEDULER_TEST2__

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

struct sched_task;
typedef struct sched_task sched_task;

typedef struct sched_list {
    sched_task *first, *last;
} sched_list;

struct sched_task {
    sched_task *sched_prev, *sched_next;
    uint32_t number;
};

void sched_expect(bool exp) {
    if(!exp) {
        printf("Expect error\r\n");
        exit(0);
    }
}

/**************************** List functions **********************************/

static void list_unlink(sched_list *list, sched_task *task) {
    if(list->first == task && list->last == task) {
        list->first = NULL;
        list->last = NULL;
        sched_expect(task->sched_prev == NULL);
        sched_expect(task->sched_next == NULL);
    } else {
        sched_task *prev = task->sched_prev;
        sched_task *next = task->sched_next;
        if(prev != NULL) prev->sched_next = next;
        if(next != NULL) next->sched_prev = prev;
        if(list->first == task) list->first = task->sched_next;
        if(list->last  == task) list->last  = task->sched_prev;
        task->sched_prev = task->sched_next = NULL;
    }
} 

static void list_append(sched_list *list, sched_task *task) {
    sched_expect(task->sched_prev == NULL);
    sched_expect(task->sched_next == NULL);
    if(list->first == NULL /* && list->last == NULL */) {
        list->first = list->last = task;
    } else {
        sched_task *prev = list->last;
        prev->sched_next = task;
        task->sched_prev = prev;
        list->last = task;
    }
}

/**
 * @brief Exchanges position of given two tasks in a list.
 * 
 * @param list Original list
 * @param cur nth task in given list
 * @param prev (n-1)th task in given list
 */
static void list_exchange(sched_list *list, sched_task *cur, sched_task *prev) {
    sched_expect(cur->sched_prev == prev);
    sched_expect(prev->sched_next == cur);

    if(list->first == prev) {
        list->first = cur;
        cur->sched_prev = NULL;
    } else {
        cur->sched_prev = prev->sched_prev;
        prev->sched_prev->sched_next = cur;
    }
    if(list->last == cur) {
        list->last = prev;
        prev->sched_next = NULL;
    } else {
        prev->sched_next = cur->sched_next;
        cur->sched_next->sched_prev = prev;
    }
    cur->sched_next = prev;
    prev->sched_prev = cur;
}

static void list_bubbleup(sched_list *list, sched_task *task, 
        bool (*is_lower)(sched_task *one, sched_task *two))
{
    sched_task *prev = task->sched_prev;
    while(prev != NULL) {
        if(is_lower(task, prev)) {
            // uart_print(" exchange");
            // uart_out((uint32_t) task);
            // uart_out((uint32_t) prev);
            // uart_print(" ");
            list_exchange(list, task, prev);
            // if(
            //     (((uint32_t) task & 0xfff) == 0x750 && ((uint32_t) prev & 0xfff) == 0x700)
            //   || (((uint32_t) task & 0xfff) == 0x778 && ((uint32_t) prev & 0x728) == 0x700)
            //     ) {
            //     uart_print("{");
            //     print_tasks();
            //     uart_print("}");
            // }
        } else break;

        prev = task->sched_prev;
    }
}

static void list_insertbefore(sched_list *list, sched_task *task, sched_task *next) {
    if(next == NULL) {
        list_append(list, task);
    } else {
        sched_task *prev = next->sched_prev;
        task->sched_prev = prev;
        if(prev) prev->sched_next = task;
        else list->first = task;

        next->sched_prev = task;
        task->sched_next = next;
    }
}

static void list_insert(sched_list *list, sched_task *task, 
        bool (*is_lower)(sched_task *one, sched_task *two)) {
    sched_task *before = NULL, *cur = list->last;
    while(cur != NULL) {
        if(!is_lower(task, cur)) break;
        before = cur;
        cur = cur->sched_prev;
    }
    list_insertbefore(list, task, before);
}


/**************************** MAIN ********************************************/

static bool is_lower(sched_task *one, sched_task *two) {
    return one->number < two->number;
}

#define NUM_TASKS (8 * 1024)
sched_task tasks[NUM_TASKS];

void check_tasks(sched_list *list, unsigned size) {
    unsigned count = 0;
    sched_task *task = list->first;
    uint32_t prev_num = 0;
    while(task != NULL) {
        uint32_t cur_num = task->number;
        if(cur_num < prev_num) {
            printf(" - %08X/%08X fail\n", prev_num, cur_num);
        } else {
            // printf(" - %08X\n", cur_num);
        }
        prev_num = cur_num;

        task = task->sched_next;
        count++;
    }

    task = list->last;
    unsigned count2 = 0;
    while(task != NULL) {
        uint32_t cur_num = task->number;
        if(cur_num > prev_num) {
            printf(" - %08X/%08X fail\n", prev_num, cur_num);
        } else {
            // printf(" - %08X\n", cur_num);
        }
        prev_num = cur_num;

        task = task->sched_prev;
        count2++;
    }

    printf("Total count %u/%u vs allocated %u\n", count, count2, size);
}

int main() {
    sched_list list = {0,0};
    
    uint32_t tm = time(0);
    printf("Seed: 0x%08X\n", tm);
    srand(tm);

    // Perform addition
    // This will take O(n^2)
    printf("Started... Constructing list!\n");
    for(unsigned i = 0; i < NUM_TASKS; i++) {
        sched_task *task = &tasks[i];
        task->sched_next = task->sched_prev = NULL;
        task->number = rand();
        list_insert(&list, task, is_lower);
    }

    // Check tasks O(n)
    check_tasks(&list, NUM_TASKS);

    // Remove random tasks O(n)
    sched_task *task = list.first;
    uint32_t removed = 0;
    while(task != NULL) {
        sched_task *next = task->sched_next;
        if(rand() & 1) {
            list_unlink(&list, task);
            removed++;
        }
        task = next;
    }

    // Check tasks O(n)
    check_tasks(&list, NUM_TASKS - removed);
}

#endif

