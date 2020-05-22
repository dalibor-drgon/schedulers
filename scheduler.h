

#ifndef __SCHEDULER_H
#define __SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>
#include "scheduler-config.h"

#ifdef __cplusplus
extern "C" {
#endif


#define SCHED_MUTEX_INIT {0}
#define SCHED_COND_INIT {{(sched_task *) 0, (sched_task *) 0}}

/// Task structure
struct sched_task;
typedef struct sched_task sched_task;

/// Scheduler structure
struct sched;
typedef struct sched sched;

/// Stack inside PendSV and SysTick exception
struct sched_stack;
typedef struct sched_stack sched_stack;

struct sched_mutex;
typedef struct sched_mutex sched_mutex;

struct sched_cond;
typedef struct sched_cond sched_cond;

typedef bool (*sched_syscall_function)(void *data, sched_task *task);
typedef void (*sched_entry_function)(void *data);

struct sched_stack {
	/// Registers pushed onto the stack by software by "STMDB x!, {r4-r11}".
    /// Pushed after the the hardware (NVIC) pushes its registers.
    struct {
        uint32_t r4;
        uint32_t r5;
        uint32_t r6;
        uint32_t r7;
        uint32_t r8;
        uint32_t r9;
        uint32_t r10;
        uint32_t r11;
    };

	/// Registers pushed on the stack by NVIC (by hardware) before we push other
	/// registers.
    struct {
        uint32_t r0;
        uint32_t r1;
        uint32_t r2;
        uint32_t r3;
        uint32_t r12;
        void *lr;
        void *pc;
        uint32_t psr;
    };
};

extern sched scheduler;

struct sched_mutex {
    /// This is filled with pointer to the currently executed task
    uint32_t value;
    // sched_mutex *prev, *next;
};

typedef struct sched_list {
    sched_task *first, *last;
} sched_list;

typedef struct sched_queue {
    sched_task *first, *last;
} sched_queue;

typedef struct sched_mutex_queue {
    sched_mutex *first, *last;
} sched_mutex_list;

struct sched_cond {
    // List containing all tasks waiting for this conditional
    sched_list tasks;
    // // Last task to be signaled. Used when sched_cond_broadcast() is used. NULL
    // // means no tasks are to be signaled.
    // sched_task *last;
};

/// Contains state of task
typedef enum sched_task_state {
    /// The task is either running or waiting to be run - check
    /// scheduler.cur_task to find out.
    SCHEDSTATE_READY = 0, 
    /// Waiting for syscall/IO/sleep to finish.
    SCHEDSTATE_BLOCKING = 1
} sched_task_state;

typedef struct sched_task_realtime {
    uint32_t next_execution;
    uint32_t interval;
} sched_task_realtime;

struct sched_task {
    union {
        struct {
            sched_task *prev, *next;
        } list;
        struct {
            sched_task *next;
        } queue;
    };

    // sched_mutex_list locked_mutexes;
    /// Tasks waiting for mutex to be released
    sched_list dependant_tasks;

    void *sp_end, *sp;
    sched_task_state state;
    uint8_t _pad0, _pad1, _pad2;
    sched_mutex *awaiting_mutex;

    union {
        sched_task_realtime realtime;
    } data;

    sched_entry_function entry_function;
    void *function_data;
};

struct sched {
    /**
     * @brief This is updated with a pointer to the task that is currently
     * executed.
     */
    sched_task *cur_task;

    /**
     * @brief Tasks using realtime (RMS or EDF) scheduler. This linked list is
     * sorted using Bubble Sort as Min-Max meaning the task next to be executed
     * is `*first` and the task last to be executed is `*last`.
     */
    sched_list realtime_tasks;

    /**
     * @brief Realtime tasks that are to be added to the `realtime_tasks` list
     * once their time comes (once task.next_execution equals to or is lower
     * than sched_ticks()).
     */
    sched_list realtime_tasks_waiting;

    /**
     * @brief (Realtime, Normal or Low-priority) tasks that were
     * blocking/waiting but the operation which caused them to block/wait
     * finished. FIFO queue.
     */
    sched_queue fired_tasks;

    /**
     * @brief Sleep thread which is switched to whenever the scheduler has no
     * tasks to be scheduled. This thread just enters loop from which it calls
     * WFI instruction to enter light Sleep mode.
     */
    sched_task sleep_task;
};

/**************************** Utilities ***************************************/

static inline uint32_t sched_irq_disable() {
    uint32_t primask;
    __asm__ volatile ("MRS %0, primask\n\t"
                      "CPSID i" : "=r" (primask) );
    return primask;
}

static inline void sched_irq_restore(uint32_t primask) {
    __asm__ volatile("MSR primask, %0" :: "r" (primask));
}

uint32_t sched_ticks();

#ifdef DEBUG
#define sched_expect(expression) do {                                          \
    if((expression) == false) sched_expect_fail(__FILE__, __LINE__);           \
} while(0)

extern void sched_expect_fail(char *file, int lineno);
#else
#define sched_expect(expression)
#endif

/**************************** Scheduler ***************************************/

void sched_init();

void sched_start();

void sched_apply();

int sched_syscall(
    sched_syscall_function syscall_function,
    void *data);

/**************************** Task functions **********************************/

void sched_task_init(sched_task *task,
        uint8_t *sp, unsigned sp_length,
        sched_entry_function function, void *data
);

void sched_task_add(sched_task *task,
        uint32_t next_execution, uint32_t interval);

void sched_task_enqueue(sched_task *task);

void sched_task_fire(sched_task *task, int return_value);

/**
 * @brief For use in realtime tasks. Use this to signal the end of the task
 * until the next execution time happens.
 * 
 * NOTE: You can either return from the task or use this function, it's up to
 * the user's liking.
 */
void sched_task_tick();

static inline sched_task *sched_task_current() {
    return scheduler.cur_task;
}

/**************************** Task privileged functions ***********************/
// Can be called from syscall

void sched_taskp_tick(sched_task *task);


/**************************** Mutex functions *********************************/

void sched_mutex_lock(sched_mutex *mutex);
bool sched_mutex_trylock(sched_mutex *mutex);
void sched_mutex_unlock(sched_mutex *mutex);

/**************************** Cond functions **********************************/

void sched_cond_wait(sched_cond *cond, sched_mutex *mutex);
void sched_cond_signal(sched_cond *cond);
void sched_cond_broadcast(sched_cond *cond);

void sched_cond_signal_fromisr(sched_cond *cond);
void sched_cond_broadcast_fromisr(sched_cond *cond);


#ifdef __cplusplus
}
#endif

#endif