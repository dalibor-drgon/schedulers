

#ifndef __SCHEDULER_H
#define __SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "scheduler-config.h"

#ifdef __cplusplus
extern "C" {
#endif


#define SCHED_MUTEX_INIT {NULL, NULL}
#define SCHED_COND_INIT {{(sched_task *) NULL, (sched_task *) NULL}}

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
    /// Task which locked the mutex and currently owns it
    sched_task *volatile owner;
    /// Last task that was added to the waiting-to-lock queue
    sched_task *volatile last_to_lock;
};

typedef struct sched_list {
    sched_task *volatile first, *volatile last;
} sched_list;

typedef struct sched_queue {
    sched_task *volatile first, *volatile last;
} sched_queue;

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
    SCHEDSTATE_BLOCKING = 1,
    /// Ended
    SCHEDSTATE_DEAD = 2
} sched_task_state;

typedef enum sched_task_list_type {
    /// The task should be moved into scheduler.realtime_tasks
    SCHEDLISTTYPE_RUNNING = 0,
    /// The task should be moved into scheduler.realtime_tasks_waiting and wait
    /// until task.next_exeuction time
    SCHEDLISTTYPE_WAITING = 1
} sched_task_list_type;

struct sched_task {
    union {
        struct {
            sched_task *volatile prev, *volatile next;
        } list;
        struct {
            sched_task *volatile next;
        } queue;
    };

    sched_task *volatile task_list_next;

    /// Tasks waiting for mutex to be released
    sched_list dependant_tasks;
    
    uint32_t sched_time;
    const char *name;
    uint64_t running_time;

    void *volatile sp;
    sched_task_state state;
    sched_task_list_type list_type;
    uint8_t priority;
    uint8_t _pad0;
    sched_mutex *volatile awaiting_mutex;

    /// If list_type == SCHEDLISTTYPE_WAITING or if this task is in
    /// scheduler.realtime_tasks_waiting list, this is used as deadline for when
    /// the task should be resumed.
    uint32_t next_execution;
};

struct sched {
    /**
     * @brief This is updated with a pointer to the task that is currently
     * executed.
     */
    sched_task *cur_task;

    /**
     * @brief First registered task.
     */
    sched_task *volatile task_list_first;

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

    /**
     * @brief Set to true once sched_start() is called.
     */
    bool is_running;
};

/**************************** Utilities ***************************************/

static inline uint32_t sched_irq_disable(void) {
    uint32_t primask;
    __asm__ volatile ("MRS %0, primask\n\t"
                      "CPSID i" : "=r" (primask) );
    return primask;
}

static inline void sched_irq_restore(uint32_t primask) {
    __asm__ volatile("MSR primask, %0" :: "r" (primask));
}

// static inline uint32_t sched_ctxs_disable() {
//     uint32_t basepri;
//     __asm__ volatile ("MRS %0, basepri_max\n\t"
//                       "MSR basepri, %1" : "=r" (basepri) : "r" (0x3) );
//     return basepri;
// }

// static inline void sched_ctxs_restore(uint32_t basepri) {
//     __asm__ volatile("MSR basepri, %0" :: "r" (basepri));
// }

uint32_t sched_ticks(void);

#ifdef DEBUG
#define sched_expect(expression) do {                                          \
    if((expression) == false) sched_expect_fail(__FILE__, __LINE__);           \
} while(0)

extern void sched_expect_fail(char *file, int lineno);
#else
#define sched_expect(expression)
#endif

#define sched_trigger_pendsv()      \
	SCB_ICSR |= SCB_ICSR_PENDSVSET;



/**************************** Scheduler ***************************************/

void sched_init(void);

void sched_start(void);

void sched_apply(void);

static inline int sched_syscall(
        sched_syscall_function syscall_function,
        void *data) {
    register unsigned r0 asm ("r0") = (unsigned) syscall_function;
    register unsigned r1 asm ("r1") = (unsigned) data;
    asm volatile 
        ("svc 0"
        : "=r" (r0) : "r" (r0), "r" (r1)
        : "memory", "cc");
    (void) r1;
    return (int) r0;
}

static inline int64_t sched_syscall64(
        sched_syscall_function syscall_function,
        void *data) {
    register unsigned r0 asm ("r0") = (unsigned) syscall_function;
    register unsigned r1 asm ("r1") = (unsigned) data;
    asm volatile 
        ("svc 0"
        : "=r" (r0), "=r" (r1) : "r" (r0), "r" (r1)
        : "memory", "cc");
    return r0 | ((int64_t) r1 << 32);
}


/**************************** Task functions **********************************/

void sched_task_init(sched_task *task, const char *name, uint8_t priority,
        uint8_t *sp, unsigned sp_length,
        sched_entry_function function, void *data
);

void sched_task_add(sched_task *task);

void sched_task_delete(void);

void sched_task_enqueue(sched_task *task);


// Set return value
static inline void sched_task_set_exit_code(sched_task *task, int return_value) {
    sched_stack *stack = (sched_stack *) task->sp;
    stack->r0 = return_value;
}

// Set return value
static inline void sched_task_set_exit_code64(sched_task *task, int64_t return_value) {
    sched_stack *stack = (sched_stack *) task->sp;
    stack->r0 = return_value;
    stack->r1 = return_value >> 32;
}

void sched_task_fire(sched_task *task, int return_value);

void sched_task_sleepuntil(uint32_t ticks);

void sched_task_sleep(uint32_t ticks);

static inline sched_task *sched_task_current(void) {
    return scheduler.cur_task;
}


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

/*************************** Monitoring functions *****************************/

uint64_t sched_monit_getruntime(sched_task *task);

#ifdef __cplusplus
}
#endif

#endif