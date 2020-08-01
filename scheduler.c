
#include "scheduler.h"
#include <stddef.h>

#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>

sched scheduler = {0};

/**************************** List functions **********************************/

static void list_unlink(sched_list *list, sched_task *task) {
    if(list->first == task && list->last == task) {
        list->first = NULL;
        list->last = NULL;
        sched_expect(task->list.prev == NULL);
        sched_expect(task->list.next == NULL);
    } else {
        sched_task *prev = task->list.prev;
        sched_task *next = task->list.next;
        if(prev != NULL) prev->list.next = next;
        if(next != NULL) next->list.prev = prev;
        if(list->first == task) list->first = task->list.next;
        if(list->last  == task) list->last  = task->list.prev;
        task->list.prev = task->list.next = NULL;
    }
} 

static void list_append(sched_list *list, sched_task *task) {
    sched_expect(task->list.prev == NULL);
    sched_expect(task->list.next == NULL);
    if(list->first == NULL /* && list->last == NULL */) {
        list->first = list->last = task;
    } else {
        sched_task *prev = list->last;
        prev->list.next = task;
        task->list.prev = prev;
        list->last = task;
    }
}

static void list_insertbefore(sched_list *list, sched_task *task, sched_task *next) {
    if(next == NULL) {
        list_append(list, task);
    } else {
        sched_task *prev = next->list.prev;
        task->list.prev = prev;
        if(prev) prev->list.next = task;
        else list->first = task;

        next->list.prev = task;
        task->list.next = next;
    }
}

static void list_insert(sched_list *list, sched_task *task, 
        bool (*is_lower)(sched_task *one, sched_task *two)) {
    sched_task *before = NULL, *cur = list->last;
    while(cur != NULL) {
        if(!is_lower(task, cur)) break;
        before = cur;
        cur = cur->list.prev;
    }
    list_insertbefore(list, task, before);
}

static sched_task *list_find_for_mutex(sched_list *list, sched_mutex *mutex) {
    sched_task *task = list->first;
    while(task != NULL) {
        if(task->awaiting_mutex == mutex) {
            break;
        }
        task = task->list.next;
    }
    return task;
}


/**************************** Queue functions *********************************/

static sched_task *queue_dequeue(sched_queue *queue) {
    uint32_t primask = sched_irq_disable();
    if(queue->first == NULL) {
        sched_irq_restore(primask);
        return NULL;
    }
    sched_task *task = queue->first;
    if(queue->first == task && queue->last == task) {
        queue->first = queue->last = NULL;
    } else {
        queue->first = task->queue.next;
    }
    task->queue.next = NULL;

    sched_irq_restore(primask);
    return task;
}

static void queue_enqueue(sched_queue *queue, sched_task *task) {
    uint32_t primask = sched_irq_disable();
    if(queue->first == NULL /* && queue->last == NULL */) {
        queue->first = queue->last = task;
    } else {
        sched_task *prev = queue->last;
        prev->queue.next = task;
        queue->last = task;
    }
    sched_irq_restore(primask);
}


/**************************** List comparators ********************************/

static bool list_rt_islower(sched_task *one, sched_task *two) {
    return one->priority > two->priority;
}

static bool list_rtw_islower(sched_task *one, sched_task *two) {
    uint32_t ticks = sched_ticks();
    int32_t rem_one = one->next_execution - ticks;
    int32_t rem_two = two->next_execution - ticks;
    return rem_one < rem_two;
}


/**************************** Utilities ***************************************/

uint32_t sched_ticks() {
  uint16_t cnt_lo;
  uint16_t cnt_hi;
  do {
    cnt_hi = TIM_CNT(SCHED_TIMhi);
    cnt_lo = TIM_CNT(SCHED_TIMlo);
  } while(cnt_lo != TIM_CNT(SCHED_TIMlo) || cnt_hi != TIM_CNT(SCHED_TIMhi));
  return ((uint32_t) cnt_hi << 16) | cnt_lo;
}

static void rcc_periph_clock_enable_tim(uint32_t TIM) {
    if(TIM == TIM1) {
        rcc_periph_clock_enable(RCC_TIM1);
    } else if(TIM == TIM2) {
        rcc_periph_clock_enable(RCC_TIM2);
    } else if(TIM == TIM3) {
        rcc_periph_clock_enable(RCC_TIM3);
    } else if(TIM == TIM4) {
        rcc_periph_clock_enable(RCC_TIM4);
    }
}

static void nvic_enable_irq_tim(uint32_t TIM) {
    const uint8_t priority = 0xff;
    if(TIM == TIM1) {
        nvic_enable_irq(NVIC_TIM1_CC_IRQ);
        nvic_set_priority(NVIC_TIM1_CC_IRQ, priority);
    } else if(TIM == TIM2) {
        nvic_enable_irq(NVIC_TIM2_IRQ);
        nvic_set_priority(NVIC_TIM2_IRQ, priority);
    } else if(TIM == TIM3) {
        nvic_enable_irq(NVIC_TIM3_IRQ);
        nvic_set_priority(NVIC_TIM3_IRQ, priority);
    } else if(TIM == TIM4) {
        nvic_enable_irq(NVIC_TIM4_IRQ);
        nvic_set_priority(NVIC_TIM4_IRQ, priority);
    }
}


/**************************** Main task ***************************************/

static uint8_t sleep_task_sp[128];

void sleep_task_entry(void *ign) {
    // Just keep entering sleep mode and wait for TIMer interrupt to wake up
    while(1) {
        asm volatile("WFI");
    }
}

/**************************** Scheduler functions *****************************/


void sched_init() {
    /** Init scheduler **/
    scheduler.realtime_tasks.first = scheduler.realtime_tasks.last = NULL;
    scheduler.fired_tasks.first = scheduler.fired_tasks.last = NULL;
    scheduler.cur_task = &scheduler.sleep_task;
    scheduler.sleep_task.state = SCHEDSTATE_READY;
    scheduler.is_running = false;

    scheduler.sleep_task.task_list_next = NULL;
    sched_task_init(&scheduler.sleep_task, 0, sleep_task_sp, sizeof(sleep_task_sp), sleep_task_entry, NULL);
    scheduler.task_list_first = &scheduler.sleep_task;

    nvic_set_priority(NVIC_PENDSV_IRQ, 0xff);

    /** Init timer **/

    // Enable RCC clock for these timers
	rcc_periph_clock_enable_tim(SCHED_TIMlo);
	rcc_periph_clock_enable_tim(SCHED_TIMhi);

    // Setup prescaler
    timer_set_prescaler(SCHED_TIMlo, SCHED_CPU_MHZ-1);

    // Setup TIM2 as Master-mode timer, TIM3 as slave-mode timer
    TIM_CR2(SCHED_TIMlo) |= TIM_CR2_MMS_UPDATE;
    switch(SCHED_ITR) {
        default:
        case 0:
            TIM_SMCR(SCHED_TIMhi) |= TIM_SMCR_TS_ITR0; 
            break;
        case 1:
            TIM_SMCR(SCHED_TIMhi) |= TIM_SMCR_TS_ITR1;
            break;
        case 2:
            TIM_SMCR(SCHED_TIMhi) |= TIM_SMCR_TS_ITR2; 
            break;
        case 3:
            TIM_SMCR(SCHED_TIMhi) |= TIM_SMCR_TS_ITR3; 
            break;
    }
    TIM_SMCR(SCHED_TIMhi) |= TIM_SMCR_SMS_ECM1;

    // Finally enable those timers
    timer_enable_counter(SCHED_TIMhi);
    timer_enable_counter(SCHED_TIMlo);

    nvic_enable_irq_tim(SCHED_TIMhi);
    nvic_enable_irq_tim(SCHED_TIMlo);
}

void uart_print(char *c);

void sched_start() {
    sched_syscall(NULL, NULL);
}

void sched_apply() {
    sched_syscall(NULL, NULL);
}

static sched_syscall_function pendsv_syscall_function;
static void * pendsv_syscall_data;

int __attribute__((noinline)) sched_syscall(
    sched_syscall_function syscall_function,
    void *data)
{
    uint32_t irq = sched_irq_disable();
    pendsv_syscall_function = syscall_function;
    pendsv_syscall_data = data;
    sched_irq_restore(irq);

	/* Trigger PendSV, causing PendSV_Handler to be called immediately */
    sched_trigger_pendsv();
	__asm__ volatile("nop");
	__asm__ volatile("nop");
	__asm__ volatile("nop");
	__asm__ volatile("nop");
    
}// Return value will be set by given syscall, do not worry about the warning. 
// Tested with gcc, may require modification with other compilers.


/**************************** Static scheduler functions **********************/

static inline void sched_setup_hi(uint16_t goal) {
    // Disable TIMlo.CC1 interrupt
    TIM_DIER(SCHED_TIMlo) &= ~TIM_DIER_CC1IE;

    // Set goal for hi timer
    TIM_CCR1(SCHED_TIMhi) = goal;
    // Clear CC1 interrupt flag
    TIM_SR(SCHED_TIMhi) &= ~TIM_SR_CC1IF;
    // Enable TIMhi.CC1 interrupt
    TIM_DIER(SCHED_TIMhi) |= TIM_DIER_CC1IE;
}

static inline void sched_setup_lo(uint16_t goal) {
    // Disable TIMhi.CC1 interrupt
    TIM_DIER(SCHED_TIMhi) &= ~TIM_DIER_CC1IE;

    // Set goal for lo timer
    TIM_CCR1(SCHED_TIMlo) = goal;
    // Clear CC1 interrupt flag
    TIM_SR(SCHED_TIMlo) &= ~TIM_SR_CC1IF;
    // Enable TIMhi.CC1 interrupt
    TIM_DIER(SCHED_TIMlo) |= TIM_DIER_CC1IE;
}

static bool sched_setup(uint32_t goal_time) {
    int32_t time, rem;
    uint32_t primask = sched_irq_disable();
    do {
        time = sched_ticks();
        rem = goal_time - time;
    } while((rem > 0xffff && rem <= 0x10002)
         || (rem > 0      && rem <= 2));
    if(rem <= 0) {
        sched_irq_restore(primask);
        return true;
    }
    if(rem > 0xffff) {
        // setup high timer
        sched_setup_hi(goal_time >> 16);
    } else {
        // setup low timer
        sched_setup_lo(goal_time & 0xffff);
    }
    sched_irq_restore(primask);
    return false;
}

static void sched_unsetup() {
    // Disable TIMlo.CC1 interrupt
    TIM_DIER(SCHED_TIMlo) &= ~TIM_DIER_CC1IE;
    // Disable TIMhi.CC1 interrupt
    TIM_DIER(SCHED_TIMhi) &= ~TIM_DIER_CC1IE;
}

static sched_task *sched_nexttask() {
    do {
        // Add fired tasks from queue into realtime_tasks or
        // realtime_tasks_waiting list 
        sched_task *task;
        do {
            // Dequeue task
            task = queue_dequeue(&scheduler.fired_tasks);
            if(task == NULL) break;
            if(task->list_type == SCHEDLISTTYPE_WAITING) {
                list_insert(&scheduler.realtime_tasks_waiting, task, list_rtw_islower);
            } else {
                list_insert(&scheduler.realtime_tasks, task, list_rt_islower);
            }
        } while(true);

        // Move no-longer-waiting tasks into task pending list `realtime_tasks`
        uint32_t ticks = sched_ticks();
        do {
            // If there are no waiting tasks, break
            task = scheduler.realtime_tasks_waiting.first;
            if(task == NULL) break;

            // If current task is still waiting, break
            int32_t rem_ticks = task->next_execution - ticks;
            if(rem_ticks > 0) break; 

            list_unlink(&scheduler.realtime_tasks_waiting, task);
            list_insert(&scheduler.realtime_tasks, task, list_rt_islower);
        } while(true);

        if(scheduler.realtime_tasks_waiting.first == NULL) {
            sched_unsetup();
            break;
        } else {
            if(!sched_setup(scheduler.realtime_tasks_waiting.first->next_execution))
                break;
        }
    } while(true);

    if(scheduler.realtime_tasks.first != NULL) {
        return scheduler.cur_task = scheduler.realtime_tasks.first;
    }
    return scheduler.cur_task = &scheduler.sleep_task;
}

static void sched_movetask() {
    list_unlink(&scheduler.realtime_tasks, scheduler.cur_task);
}


/**************************** Task privileged functions ***********************/
// Can be run only at initialization phase or from syscall

static void sched_taskp_reinit(sched_task *task, sched_entry_function entry_function,
        void *function_data, void *sp_end) {
    task->sp = (char*) sp_end - sizeof(sched_stack);

    sched_stack *stack = (sched_stack *) task->sp;
    stack->psr = 0x21000000;
    stack->pc = entry_function;
    stack->lr = sched_task_delete;
    stack->r0 = (uint32_t) function_data;
}

/**************************** Task built-in syscalls **************************/

static bool sched_task_delete_syscall(void *data, sched_task *cur_task) {
    (void) data;
    cur_task->state = SCHEDSTATE_DEAD;

    // Remove the task from list O(n)
    sched_task *prev = NULL;
    sched_task *cur = scheduler.task_list_first;
    while(cur != NULL) {
        if(cur == cur_task) break;
        prev = cur;
        cur = cur->task_list_next;
    }
    sched_expect(cur != NULL);
    prev->task_list_next = cur_task->task_list_next;
    cur_task->task_list_next = NULL;

    return false;
}

static bool sched_task_sleepuntil_syscall(void *data, sched_task *cur_task) {
    uint32_t goal = (uint32_t) data;
    cur_task->next_execution = goal;
    list_insert(&scheduler.realtime_tasks_waiting, cur_task, list_rtw_islower);
    return false;
}

static bool sched_task_add_syscall(void *data, sched_task *cur_task) {
    sched_task *task = (sched_task *) data;

    sched_task *next = scheduler.task_list_first;
    task->task_list_next = next;
    scheduler.task_list_first = task;
    list_insert(&scheduler.realtime_tasks, task, list_rt_islower);

    return false;
}

static bool sched_mutex_lock_syscall(void *data, sched_task *cur_task) {
    sched_mutex *mutex = (sched_mutex *) data;
    sched_task *locked_task = mutex->last_to_lock;
    if(locked_task == NULL) {
        mutex->last_to_lock = cur_task;
        mutex->owner = cur_task;
        // mutex_list_append(&cur_task->locked_mutexes, mutex);
        return true;
    }

    mutex->last_to_lock = cur_task;
    // mutex->owner is already set to the first mutex that called
    // sched_mutex_lock function.
    cur_task->awaiting_mutex = mutex;
    list_append(&locked_task->dependant_tasks, cur_task);

    return false;
}

static bool sched_mutex_unlock_syscall(void *data, sched_task *task) {
    sched_mutex *mutex = (sched_mutex *) data;
    task = mutex->owner;

    sched_task *resumed_task = list_find_for_mutex(&task->dependant_tasks, mutex);
    sched_expect(resumed_task != NULL);
    mutex->owner = resumed_task;
    list_unlink(&task->dependant_tasks, resumed_task);

    list_insert(&scheduler.realtime_tasks, resumed_task, list_rt_islower);
    list_insert(&scheduler.realtime_tasks, task, list_rt_islower);

    return false;
}

typedef struct {
    sched_cond *cond;
    sched_mutex *mutex;
} sched_cond_mutex_pair;

static bool sched_cond_wait_syscall(void *data, sched_task *cur_task) {
    sched_cond_mutex_pair *pair = (sched_cond_mutex_pair *) data;
    sched_cond *cond = pair->cond;
    sched_mutex *mutex = pair->mutex;

    uint32_t primask = sched_irq_disable();
    // Add current task to the conditional list
    list_append(&cond->tasks, cur_task);
    sched_irq_restore(primask);

    sched_task * cur_value = mutex->last_to_lock;
    sched_task * expected_value = cur_task;
    if(cur_value == expected_value) {
        // Just change the variable
        mutex->last_to_lock = NULL;
        mutex->owner = NULL;
    } else {
        // Unlock dependant task
        sched_task *resumed_task = list_find_for_mutex(&cur_task->dependant_tasks, mutex);
        sched_expect(resumed_task != NULL);
        mutex->owner = resumed_task;
        list_unlink(&cur_task->dependant_tasks, resumed_task);

        list_insert(&scheduler.realtime_tasks, resumed_task, list_rt_islower);
    }
    return false;
}

static bool sched_cond_signal_syscall(void *data, sched_task *cur_task) {
    sched_cond *cond = (sched_cond *) data;
    uint32_t primask = sched_irq_disable();
    sched_task *resumed_task = cond->tasks.first;
    if(resumed_task == NULL) {
        sched_irq_restore(primask);
        // Done
    } else {
        list_unlink(&cond->tasks, resumed_task);
        sched_irq_restore(primask);
        list_insert(&scheduler.realtime_tasks, resumed_task, list_rt_islower);
    }
    return true;
}

static bool sched_cond_broadcast_syscall(void *data, sched_task *cur_task) {
    sched_cond *cond = (sched_cond *) data;
    uint32_t primask = sched_irq_disable();
    sched_task *resumed_task;
    while((resumed_task = cond->tasks.first) != NULL) {
        list_unlink(&cond->tasks, resumed_task);
        sched_irq_restore(primask);
        list_insert(&scheduler.realtime_tasks, resumed_task, list_rt_islower);
        primask = sched_irq_disable();
    }
    sched_irq_restore(primask);
    return true;
}

/**************************** Cond functions from ISR *************************/

void sched_cond_signal_fromisr(sched_cond *cond) {
    uint32_t primask = sched_irq_disable();
    sched_task *resumed_task = cond->tasks.first;
    if(resumed_task == NULL) {
        sched_irq_restore(primask);
        // Done
    } else {
        list_unlink(&cond->tasks, resumed_task);
        sched_irq_restore(primask);
        sched_task_enqueue(resumed_task);
    }
}

void sched_cond_broadcast_fromisr(sched_cond *cond) {
    uint32_t primask = sched_irq_disable();
    sched_task *resumed_task;
    while((resumed_task = cond->tasks.first) != NULL) {
        list_unlink(&cond->tasks, resumed_task);
        sched_irq_restore(primask);
        sched_task_enqueue(resumed_task);
        primask = sched_irq_disable();
    }
    sched_irq_restore(primask);
}


/**************************** Task functions **********************************/


void sched_task_init(sched_task *task, uint8_t priority,
        uint8_t *sp, unsigned sp_length,
        sched_entry_function function, void *data
) {
    task->priority = priority;
    task->state = SCHEDSTATE_DEAD;
    sched_taskp_reinit(task, function, data, sp + sp_length);

    task->awaiting_mutex = NULL;
    // task->locked_mutexes.first = task->locked_mutexes.last = NULL;

    task->list.prev = task->list.next = task->queue.next = NULL;
}

void sched_task_add(sched_task *task) {
    if(scheduler.is_running) {
        sched_syscall(sched_task_add_syscall, task);
    } else {
        sched_task *next = scheduler.task_list_first;
        task->task_list_next = next;
        scheduler.task_list_first = task;
        sched_task_enqueue(task);
    }
}

void sched_task_delete() {
    sched_syscall(sched_task_delete_syscall, NULL);
}

void sched_task_enqueue(sched_task *task) {
    // Set state to READY
    task->list_type = SCHEDLISTTYPE_RUNNING;
    task->state = SCHEDSTATE_READY;

    // Finally enqueue it into fired tasks queue to be processed from PendSV
    // handler 
    queue_enqueue(&scheduler.fired_tasks, task);
}

void sched_task_fire(sched_task *task, int return_value) {
    // Set return value
    sched_stack *stack = (sched_stack *) task->sp;
    stack->r0 = return_value;

    sched_task_enqueue(task);
}

void sched_task_sleepuntil(uint32_t ticks) {
    sched_syscall(sched_task_sleepuntil_syscall, (void *) ticks);
}

void sched_task_sleep(uint32_t ticks) {
    sched_task_sleepuntil(sched_ticks() + ticks);
}

/**************************** Mutex functions *********************************/

void sched_mutex_lock(sched_mutex *mutex) {
    sched_task * new_mutex_value = scheduler.cur_task;
    uint32_t irq = sched_irq_disable();
    sched_task * prev_value = mutex->last_to_lock;
    if(prev_value == NULL) {
        mutex->last_to_lock = new_mutex_value;
        mutex->owner = new_mutex_value;
        sched_irq_restore(irq);
    } else {
        sched_irq_restore(irq);
        sched_syscall(sched_mutex_lock_syscall, mutex);
    }
}

bool sched_mutex_trylock(sched_mutex *mutex) {
    sched_task * new_mutex_value = scheduler.cur_task;
    uint32_t irq = sched_irq_disable();
    sched_task * prev_value = mutex->last_to_lock;
    if(prev_value == NULL) {
        mutex->last_to_lock = new_mutex_value;
        mutex->owner = new_mutex_value;
        sched_irq_restore(irq);
        return true;
    } else {
        sched_irq_restore(irq);
        return false;
    }
}

void sched_mutex_unlock(sched_mutex *mutex) {
    sched_task * expected_value = mutex->owner;
    uint32_t irq = sched_irq_disable();
    sched_task * cur_val = mutex->last_to_lock;
    if(cur_val == expected_value) {
        mutex->last_to_lock = NULL;
        mutex->owner = NULL;
        sched_irq_restore(irq);
    } else {
        sched_irq_restore(irq);
        sched_syscall(sched_mutex_unlock_syscall, mutex);
    }
}


/**************************** Cond functions **********************************/

void sched_cond_wait(sched_cond *cond, sched_mutex *mutex) {
    sched_cond_mutex_pair pair = {.cond = cond, .mutex = mutex};
    // This syscall will block until either signal or broadcast is performed
    sched_syscall(sched_cond_wait_syscall, &pair);
    // It will then wake up and try to acquire this mutex
    sched_mutex_lock(mutex);
}

void sched_cond_signal(sched_cond *cond) {
    sched_syscall(sched_cond_signal_syscall, cond);
}

void sched_cond_broadcast(sched_cond *cond) {
    sched_syscall(sched_cond_broadcast_syscall, cond);
}

/**************************** Timer handlers **********************************/

void SCHED_TIMlo_IRQHandler() {
    sched_trigger_pendsv();
}

void SCHED_TIMhi_IRQHandler() {
    if(scheduler.realtime_tasks_waiting.first == NULL) {
        sched_unsetup();
    } else {
        if(!sched_setup(scheduler.realtime_tasks_waiting.first->next_execution)) {
            sched_trigger_pendsv();
        }
    }
}


/**************************** PendSV handler **********************************/

static sched_task *sched_handle_syscall() {
    // sched_stack *stack = (sched_stack *) scheduler.cur_task->sp;
    // sched_syscall_function syscall_func = (sched_syscall_function) stack->r0;
    // void * data = (void *) stack->r1;
    sched_syscall_function syscall_func = pendsv_syscall_function;
    pendsv_syscall_function = NULL;
    void * data = pendsv_syscall_data;
    if(syscall_func != NULL) {
        sched_task *cur_task = scheduler.cur_task;
        cur_task->state = SCHEDSTATE_BLOCKING;
        sched_movetask();
        if(syscall_func(data, scheduler.cur_task) == true) {
            scheduler.cur_task->state = SCHEDSTATE_READY;
            list_insertbefore(&scheduler.realtime_tasks, scheduler.cur_task, scheduler.realtime_tasks.first);
        }
    }
    return sched_nexttask();
}

void __attribute__((__naked__)) PendSV_Handler() {

	/* 0. NVIC has already pushed some registers on the program/main stack.
	 * We are free to modify R0..R3 and R12 without saving them again, and
	 * additionally the compiler may choose to use R4..R11 in this function.
	 * If it does so, the naked attribute will prevent it from saving those
	 * registers on the stack, so we'll just have to hope that it doesn't do
	 * anything with them before our stm or after our ldm instructions.
	 * Luckily, we don't ever intend to return to the original caller on the
	 * main stack, so this question is moot. */

	/* Read the link register */
	uint32_t lr;
	__asm__("MOV %0, lr" : "=r" (lr));

	if (lr & 0x4) {
		/* This PendSV call was made from a task using the PSP */

		/* 1. Push all other registers (R4..R11) on the program stack */
		void *psp;
		__asm__(
			/* Load PSP to a temporary register */
			"MRS %0, psp\n"
			/* Push context relative to the address in the temporary
			 * register, update register with resulting address */
			"STMDB %0!, {r4-r11}\n"
			/* Put back the new stack pointer in PSP (pointless) */
			"MSR psp, %0\n"
			: "=r" (psp));

		/* 2. Store that PSP in the current TCB */
	    scheduler.cur_task->sp = psp;
	} else {
		/* This PendSV call was made from a task using the MSP. Don't do
		anything, stack does not have to be saved since we will never context
		switch to a task using MSP, only PSP. */
        scheduler.is_running = true;
    }

	/* 3. Call context switch function, changes current TCB */
    sched_task *task = sched_handle_syscall();

    /* 4. Load PSP from TCB */
    void *psp = task->sp;
    /* 5. Pop R4..R11 from the program stack */
    __asm__(
        "LDMIA %0!, {r4-r11}\n"
        "MSR psp, %0\n"
        :: "r" (psp));

    // Finally, return. NVIC will pop registers from stack we set up and jump
    // where PC points to.
    __asm__("bx %0" :: "r"(0xfffffffd));
}
