
#include "scheduler.h"
#include <stddef.h>

#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>

sched scheduler;

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
            list_exchange(list, task, prev);
        } else break;

        prev = task->sched_prev;
    }
}

static sched_task *list_find_for_mutex(sched_list *list, sched_mutex *mutex) {
    sched_task *task = list->first;
    while(task != NULL) {
        if(task->awaiting_mutex == mutex) {
            break;
        }
        task = task->sched_next;
    }
    return task;
}

#if 0
static void list_bubbledown(sched_list *list, sched_task *task, 
        bool (*is_lower)(sched_task *one, sched_task *two)) 
{
    /// This sequence is same as bubble down, but the performance may be better
    /// or worse, depending on the variance of the intervals.
    list_unlink(list, task);
    list_append(list, task);
    list_bubbleup(list, task, is_lower);
}
#endif


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
        queue->first = task->fired_next;
    }
    task->fired_next = NULL;

    sched_irq_restore(primask);
    return task;
}

static void queue_enqueue(sched_queue *queue, sched_task *task) {
    uint32_t primask = sched_irq_disable();
    if(queue->first == NULL /* && queue->last == NULL */) {
        queue->first = queue->last = task;
    } else {
        sched_task *prev = queue->last;
        prev->fired_next = task;
        queue->last = task;
    }
    sched_irq_restore(primask);
}


/**************************** List comparators ********************************/

static bool list_rt_islower(sched_task *one, sched_task *two) {
    // if(one->state == SCHEDSTATE_READY && two->state != SCHEDSTATE_READY) {
    //     return true;
    // }
    // if(one->state != SCHEDSTATE_READY && two->state == SCHEDSTATE_READY) {
    //     return false;
    // }
#ifdef SCHEDULER_USE_RMS
    uint32_t interval_one = one->data.realtime.interval;
    uint32_t interval_two = two->data.realtime.interval;
    return (interval_one < interval_two);
#else 
    uint32_t deadline_one = one->data.realtime.next_execution + one->data.realtime.interval;
    uint32_t deadline_two = two->data.realtime.next_execution + two->data.realtime.interval;
    uint32_t ticks = sched_ticks();
    int32_t rem_one = deadline_one - ticks;
    int32_t rem_two = deadline_two - ticks;
    return (rem_one < rem_two);
#endif
}

static bool list_rtw_islower(sched_task *one, sched_task *two) {
    uint32_t ticks = sched_ticks();
    int32_t rem_one = one->data.realtime.next_execution - ticks;
    int32_t rem_two = two->data.realtime.next_execution - ticks;
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

/**************************** Scheduler functions *****************************/


void sched_init() {
    /** Init scheduler **/
    scheduler.realtime_tasks.first = scheduler.realtime_tasks.last = NULL;
    scheduler.fired_tasks.first = scheduler.fired_tasks.last = NULL;
    scheduler.cur_task = &scheduler.main_task;
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

void sched_start() {
    sched_syscall(NULL, NULL);
}

int __attribute__((noinline)) sched_syscall(
    sched_syscall_function syscall_function,
    void *data)
{
	/* Trigger PendSV, causing pend_sv_handler to be called immediately */
	SCB_ICSR |= SCB_ICSR_PENDSVSET;
	__asm__ volatile("nop");
	__asm__ volatile("nop");
	__asm__ volatile("nop");
	__asm__ volatile("nop");
    (void) syscall_function, (void) data;
    
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

static bool sched_nexttask() {
    do {
        // Add fired tasks from queue into realtime_tasks_waiting list
        sched_task *task;
        do {
            // Dequeue task
            task = queue_dequeue(&scheduler.fired_tasks);
            if(task == NULL) break;
            list_append(&scheduler.realtime_tasks_waiting, task);
            list_bubbleup(&scheduler.realtime_tasks_waiting, task, list_rtw_islower);
        } while(true);

        // Move no-longer-waiting tasks into task pending list `realtime_tasks`
        uint32_t ticks = sched_ticks();
        do {
            // If there are no waiting tasks, break
            task = scheduler.realtime_tasks_waiting.first;
            if(task == NULL) break;

            // If current task is still waiting, break
            int32_t rem_ticks = task->data.realtime.next_execution - ticks;
            if(rem_ticks > 0) break; 

            list_unlink(&scheduler.realtime_tasks_waiting, task);
            list_append(&scheduler.realtime_tasks, task);
            list_bubbleup(&scheduler.realtime_tasks, task, list_rt_islower);
        } while(true);

        if(scheduler.realtime_tasks_waiting.first == NULL) {
            sched_unsetup();
            break;
        } else {
            if(!sched_setup(scheduler.realtime_tasks_waiting.first->data.realtime.next_execution))
                break;
        }
    } while(true);

    if(scheduler.realtime_tasks.first == NULL) return false;
    
    // Pick process
    scheduler.cur_task = scheduler.realtime_tasks.first;
    int32_t rem = scheduler.cur_task->data.realtime.next_execution - sched_ticks();
    return rem <= 0;
}

static void sched_movetask() {
    list_unlink(&scheduler.realtime_tasks, scheduler.cur_task);
}

static void sched_restoretask() {
    // if(scheduler.cur_task->state == SCHEDSTATE_READY) {
        list_append(&scheduler.realtime_tasks, scheduler.cur_task);
        list_bubbleup(&scheduler.realtime_tasks, scheduler.cur_task, list_rt_islower);
    // }
}


static void sched_ontimhi() {
    while(sched_nexttask() == false);
}

static void sched_ontimlo() {
    while(sched_nexttask() == false);
}

/**************************** Task privileged functions ***********************/
// Can be run only at initialization phase or from syscall

static void return_function();

static void sched_taskp_reinit(sched_task *task) {
    task->sp = task->sp_end - sizeof(sched_stack);

    sched_stack *stack = (sched_stack *) task->sp;
    stack->psr = 0x21000000;
    stack->pc = task->entry_function;
    stack->lr = return_function;
    stack->r0 = (uint32_t) task->function_data;
}

void sched_taskp_tick(sched_task *task) {
    task->data.realtime.next_execution += task->data.realtime.interval;
    // list_bubbledown(&scheduler.realtime_tasks, scheduler.realtime_tasks.first,
    //         list_rt_islower);
}

/**************************** Task built-in syscalls **************************/

static bool sched_task_tick_reinit_syscall(void *data, sched_task *task) {
    (void) data;
    task->state = SCHEDSTATE_READY;
    sched_taskp_tick(task);
    sched_taskp_reinit(task);
    sched_task_enqueue(task);
    return false;
}

static bool sched_task_tick_syscall(void *data, sched_task *task) {
    (void) data;
    task->state = SCHEDSTATE_READY;
    sched_taskp_tick(task);
    sched_task_fire(task, 0);
    return false;
}

static bool sched_mutex_lock_syscall(void *data, sched_task *cur_task) {
    sched_mutex *mutex = (sched_mutex *) data;
    sched_task *locked_task = (sched_task *) mutex->value;
    if(locked_task == NULL) {
        mutex->value = (uint32_t) cur_task;
        // mutex_list_append(&cur_task->locked_mutexes, mutex);
        return true;
    }

    mutex->value = (uint32_t) cur_task;
    cur_task->awaiting_mutex = mutex;
    list_append(&locked_task->dependant_tasks, cur_task);

    return false;
}

static bool sched_mutex_unlock_syscall(void *data, sched_task *task) {
    sched_mutex *mutex = (sched_mutex *) data;

    sched_task *resumed_task = list_find_for_mutex(&task->dependant_tasks, mutex);

    sched_expect(resumed_task != NULL);

    // mutex_list_unlink(&task->locked_mutexes, mutex);
    // mutex_list_append(&resumed_task->locked_mutexes, mutex);

    list_unlink(&task->dependant_tasks, resumed_task);

    // queue_enqueue(&scheduler.fired_tasks, resumed_task);
    list_append(&scheduler.realtime_tasks, resumed_task);
    list_bubbleup(&scheduler.realtime_tasks, resumed_task, list_rt_islower);
    // queue_enqueue(&scheduler.fired_tasks, task);
    list_append(&scheduler.realtime_tasks, task);
    list_bubbleup(&scheduler.realtime_tasks, task, list_rt_islower);

    return false;
}

/**************************** Task functions **********************************/

void sched_task_tick() {
    sched_syscall(sched_task_tick_syscall, NULL);
}

void sched_task_init(sched_task *task,
        uint8_t *sp, unsigned sp_length,
        sched_entry_function function, void *data
) {
    task->sp_end = sp + sp_length;
    task->entry_function = function;
    task->function_data = data;
    sched_taskp_reinit(task);

    task->awaiting_mutex = NULL;
    // task->locked_mutexes.first = task->locked_mutexes.last = NULL;

    task->sched_prev = task->sched_next = task->fired_next = NULL;
}

void sched_task_add(sched_task *task,
        uint32_t next_execution, uint32_t interval) {
    task->state = SCHEDSTATE_READY;
    task->data.realtime.next_execution = next_execution;
    task->data.realtime.interval = interval;
    queue_enqueue(&scheduler.fired_tasks, task);
    // list_append(&scheduler.realtime_tasks, task);
    // list_bubbleup(&scheduler.realtime_tasks, task, list_rt_islower);
}

void sched_task_enqueue(sched_task *task) {
    // Set state to READY
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

/**************************** Mutex functions *********************************/

void sched_mutex_lock(sched_mutex *mutex) {
    uint32_t new_mutex_value = (uint32_t) scheduler.cur_task;
    uint32_t irq = sched_irq_disable();
    uint32_t prev_value = mutex->value;
    if(prev_value == 0) {
        mutex->value = new_mutex_value;
        // mutex_list_append(&scheduler.cur_task->locked_mutexes, mutex);
        sched_irq_restore(irq);
    } else {
        sched_irq_restore(irq);
        sched_syscall(sched_mutex_lock_syscall, mutex);
    }
}

bool sched_mutex_trylock(sched_mutex *mutex) {
    uint32_t new_mutex_value = (uint32_t) scheduler.cur_task;
    uint32_t irq = sched_irq_disable();
    uint32_t prev_value = mutex->value;
    if(prev_value == 0) {
        mutex->value = new_mutex_value;
        // mutex_list_append(&scheduler.cur_task->locked_mutexes, mutex);
        sched_irq_restore(irq);
        return true;
    } else {
        sched_irq_restore(irq);
        return false;
    }
}

void sched_mutex_unlock(sched_mutex *mutex) {
    uint32_t expected_value = (uint32_t) scheduler.cur_task;
    uint32_t irq = sched_irq_disable();
    uint32_t cur_val = mutex->value;
    if(cur_val == expected_value) {
        mutex->value = 0;
        // mutex_list_unlink(&scheduler.cur_task->locked_mutexes, mutex);
        sched_irq_restore(irq);
    } else {
        sched_irq_restore(irq);
        sched_syscall(sched_mutex_unlock_syscall, mutex);
    }
}


/**************************** Timer handlers **********************************/

void __attribute__((__naked__)) SCHED_TIMlo_IRQHandler() {
    const uint32_t RETURN_ON_PSP = 0xfffffffd;

	/* 0. NVIC has already pushed some registers on the program/main stack.
	 * We are free to modify R0..R3 and R12 without saving them again, and
	 * additionally the compiler may choose to use R4..R11 in this function.
	 * If it does so, the naked attribute will prevent it from saving those
	 * registers on the stack, so we'll just have to hope that it doesn't do
	 * anything with them before our stm or after our ldm instructions.
	 * Luckily, we don't ever intend to return to the original caller on the
	 * main stack, so this question is moot. */

    do {
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
    } while(0);
    
	/* 3. Handle the interrupt. Internvally it will call context switch
	   function, which changes current TCB */ 
    sched_ontimlo();

	/* 4. Load PSP from TCB */
	/* 5. Pop R4..R11 from the program stack */
	void *psp = scheduler.cur_task->sp;
	__asm__(
		"LDMIA %0!, {r4-r11}\n"
		"MSR psp, %0\n"
		:: "r" (psp));

    // Finally, return. NVIC will pop registers from stack we set up and jump
    // where PC points to.
	__asm__("bx %0" :: "r"(RETURN_ON_PSP));

}

void __attribute__((__naked__)) SCHED_TIMhi_IRQHandler() {
    const uint32_t RETURN_ON_PSP = 0xfffffffd;

	/* 0. NVIC has already pushed some registers on the program/main stack.
	 * We are free to modify R0..R3 and R12 without saving them again, and
	 * additionally the compiler may choose to use R4..R11 in this function.
	 * If it does so, the naked attribute will prevent it from saving those
	 * registers on the stack, so we'll just have to hope that it doesn't do
	 * anything with them before our stm or after our ldm instructions.
	 * Luckily, we don't ever intend to return to the original caller on the
	 * main stack, so this question is moot. */

    do {
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
    } while(0);
    
	/* 3. Handle the interrupt. Internvally it will call context switch
	   function, which changes current TCB */ 
    sched_ontimhi();

	/* 4. Load PSP from TCB */
	/* 5. Pop R4..R11 from the program stack */
	void *psp = scheduler.cur_task->sp;
	__asm__(
		"LDMIA %0!, {r4-r11}\n"
		"MSR psp, %0\n"
		:: "r" (psp));

    // Finally, return. NVIC will pop registers from stack we set up and jump
    // where PC points to.
	__asm__("bx %0" :: "r"(RETURN_ON_PSP));

}

/**************************** PendSV handler **********************************/

static void return_function() {
    sched_syscall(sched_task_tick_reinit_syscall, NULL);
}

static void sched_handle_syscall() {
    sched_stack *stack = (sched_stack *) scheduler.cur_task->sp;
    sched_syscall_function syscall_func = (sched_syscall_function) stack->r0;
    void * data = (void *) stack->r1;
    if(syscall_func != NULL) {
        scheduler.cur_task->state = SCHEDSTATE_BLOCKING;
        sched_movetask();
        if(syscall_func(data, scheduler.cur_task) == false) {
            while(sched_nexttask() == false);
        } else {
            sched_restoretask();
        }
    } else {
        while(sched_nexttask() == false);
    }
}

void __attribute__((__naked__)) PendSV_Handler() {
    const uint32_t RETURN_ON_PSP = 0xfffffffd;

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
		/* This PendSV call was made from a task using the MSP. This
		 * code is not equipped to return to the main task, but we store
		 * the proper registers here anyway for good form. */

		/* 1. Push all other registers (R4..R11) on the main stack */
        void *sp;
		__asm__(
			/* Push context on main stack */
			"STMDB SP!, {r4-r11}\n\t"
            "MOV %0, SP" 
            : "=r" (sp));

		/* 2. Store that PSP in the current TCB */
		scheduler.cur_task->sp = sp;
	}

	/* 3. Call context switch function, changes current TCB */
    sched_handle_syscall();

	/* 4. Load PSP from TCB */
	/* 5. Pop R4..R11 from the program stack */
	void *psp = scheduler.cur_task->sp;
	__asm__(
		"LDMIA %0!, {r4-r11}\n"
		"MSR psp, %0\n"
		:: "r" (psp));

    // Finally, return. NVIC will pop registers from stack we set up and jump
    // where PC points to.
	__asm__("bx %0" :: "r"(RETURN_ON_PSP));
}
