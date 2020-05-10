
This is simple real-time only scheduler targetting ARM Cortex M3 using just
PendSV exception with the support for system calls. 
Internally uses bubble sort with double-linked lists for maximum
performance (for small ~8 and very small ~2 number of tasks this outperforms
heap and is also a bit simpler both for usage and implementation-wise).
The user can choose between lazy EDF (earliest deadline first scheduling) or
lazy RMS (rate monotomic scheduling) - it
does not pre-empt already running tasks, performs context switch only when
currently running task finishes. With specific configuration, it is possible
that the user won't notice any difference between lazy version and normal
version of these algorithms. Since a task must finish before another is
scheduled, no mutexes nor conditionals are needed for synchronization to avoid
race conditions.

# Usage

## Dependencies

First implement following functions:

```c
extern uint32_t sched_ticks();
extern int sched_irq_disable();
extern void sched_irq_enable();
```

The first function `sched_ticks()` has to return number of ticks (number of
microseconds, milliseconds or whatnot). If you return milliseconds, the
arguments in `sched_task_add()` will be in milliseconds, if this function
returns time in microseconds, the arguments in `sched_task_add()` will be in
microseconds and so on.

Functions `sched_irq_disable()` and `sched_irq_enable()` can be implemented in
following fashion:

```c
int sched_irq_disable() {
  uint32_t result;
  __asm__ volatile ("MRS %0, primask\n\t"
                    "CPSID i" : "=r" (result) );
  return result & 1;

}

void sched_irq_enable() {
    __asm__ volatile("CPSIE i");
}
```

or, if you are using `armcc`, you can simply replace them with `__disable_irq()`
and `__enable_irq()` functions.


## Initialization

First, make sure to call `sched_init()` to initialize the scheduler.

Then you have to allocate each task you wish to run and a stack (that has to be
aligned to 8 bytes) and add each task into the scheduler manually.

```c
void enter1(void *arg) {
    uart_print("A");
    // Do something usefull
}

...
    static sched_task task1;
    static uint8_t __attribute__((aligned(8))) stack1[256];

    // Run task #1 every 100,000 microseconds (assuming sched_ticks() resolution is microseconds)
    sched_task_init(&task1, /* stack */ stack1, /* stack size */ sizeof(stack1), /* entry function */ enter1, /* entry function argument */ NULL);
    sched_task_add(&task1, /* first execution at which time */ sched_ticks(), /* interval */ 100e3);
```

Once the scheduler is initialized and contains tasks, run `sched_start()` to
hand the control over to the scheduler. It will start scheduling and running
tasks you added to the list.

The full example code may look like this:

```c
#include <scheduler.h>
#include <stdint.h>

// Scheduler
sched_task task1;
sched_task task2;

uint8_t __attribute__((aligned(8))) stack1[256];
uint8_t __attribute__((aligned(8))) stack2[256];

void enter1(void *arg) {
    uart_print("A");
    // by returning you automatically call sched_task_tick();
}

void enter2(void *arg) {
    while(1) {
        uart_print("B");
        // or you can call the sched_task_tick() from inside the program
        sched_task_tick();
    }
}

void sched_doinit() {
    sched_init();
    // Run task #1 every 100,000 microseconds
    sched_task_init(&task1, stack1, 256, enter1, NULL);
    sched_task_add(&task1, 0, 100e3);
    // Run task #2 every 200,000 microseconds
    sched_task_init(&task2, stack2, 256, enter2, NULL);
    sched_task_add(&task2, 0, 200e3);
}

int main() {
    // First initialize peripherals, clocks, etc.
    ...

    // Finally initialize scheduler
    sched_doinit();

    // And start the scheduler
    sched_start();

}

```

## System calls

To perform a system call, call `sched_syscall()`:

```c
int sched_syscall(
    sched_syscall_function syscall_function,
    void *data);
```

The system call must be implemented in the following format:

```c
#include <stdbool.h>

typedef bool (*sched_syscall_function)(void *data, sched_task *task);
```

and must return `true` if the system call was non-blocking, otherwise if it was
blocking it must return `false` and later, once it finishes, call
`sched_task_fire()` with given task and return value.

```c
void sched_task_fire(sched_task *task, int return_value);
```

For example, you can reimplement your UART output to use system calls, and once
it finishes, you can call `sched_task_fire()` (even from interrupt), and the resumed
task will be scheduled to run asap. You can reimplement basically anything that
blocks and fires interrupt once it finishes, e.g. SPI, I2C, sleep() etc.
