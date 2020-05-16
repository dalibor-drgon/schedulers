
This is simple real-time only scheduler targetting ARM Cortex M3 implemented
using PendSV exceptions and TIMers with the support for system calls and
synchronization.
Internally uses bubble sort with double-linked lists for maximum
performance (for small ~8 and very small ~2 number of tasks this outperforms
heap and is also a bit simpler both for usage and implementation-wise and does
not require allocation of any arrays or other structures except of tasks themselves).
The user can choose between EDF (earliest deadline first scheduling) or
RMS (rate monotomic scheduling). Tasks with greater priority will pre-empt
already running tasks, so mutexes should be used for synchronization.

# Usage

## Dependencies

For initialization, this library uses [libopencm3](https://libopencm3.org/)
library, but the code should be easy to port to work with other libraries as
well.

Make sure to check and edit macros in `scheduler-config.h` file.

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


## Timer

For its own use, this library cascades two configurable 16bit timers to be used
as single 32bit timer for scheduling and context-switching. You can obtain the
current time in microseconds with the function

```c
uint32_t sched_ticks();
```

This can cover time from -35 minutes ago to 35 minutes in future, and allowing
for task interval to be up to 17 minutes long.


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


## Mutexes

To achieve synchronization between tasks, you can use pthread-like mutexes. They
are also cheap if only one task uses them at a time, otherwise a short syscall
is generated. 

To start allocate a single mutex:

```c
sched_mutex mutex = SCHED_MUTEX_INIT;
```

The usage is same as pthreads' mutexes. If you don't know how to use 
mutexes, you should probably find some guide to get the gist. Basically,
whenever you enter critical section that can be executed only by one thread at a
time, you use mutexes. Before entering the critical section, you lock the mutex
with
```c
sched_mutex_lock(&mutex);
```
and once you finish, you have to unlock the mutex to let other threads in.
```c
sched_mutex_unlock(&mutex);
```

Optionally, you can use `sched_mutex_trylock(sched_mutex *mutex)`:

```c
if(sched_mutex_trylock(&mutex)) {
    // mutex acquired, do some stuff...
    sched_mutex_unlock(&mutex);
} else {
    // failed to acquire the mutex
}
```

## Example usage: UART send

Let's say you wish to re-implement UART to be blocking. This can be easily done
if you already have working implementation using interrupts or DMA that can
signalize end with custom callback.

Specifically, you have to implement two functions. One - syscall - to start the transmission, and
second one - the callback - to resume the blocking thread. Optionally, if you
wish to use this syscall from multiple threads, you should put calls to it around
mutex.

The syscall itself can be implemented in the following fashion:

```c
/// This function should start transmission of string stored in `str` of length 
/// `length` and once it finishes, the `callback` should be called.
extern void uart_start_transmission(char *str, unsigned length, void (*callback)());

/// Task to be resumed once uart transmission finishes
static sched_task *task_to_resume;

/// Syscall that sets-up the uart to start transmission for given task
bool uart_print_syscall(void *data, sched_task *task) {
    char *str = (char *) data;
    // Save the pointer to the current task
    task_to_resume = task;
    // And fire up the uart
    uart_start_transmission(str, strlen(str), uart_on_finish);
    return false; // false means this thread blocks
}
```

... and the callback:

```c
void uart_on_finish() {
    /// Resume the thread with error code 0
    sched_task_fire(task_to_resume, 0);
}
```

You can use this added functionality together with `sched_syscall()` function:

```c
    char *strng = "Hello world!";
    int err = sched_syscall(uart_print_syscall, strng);
    if(err != 0) { /*/ something went wrong */ }
```

You can make it thread-safe using mutexes and optionally make helper function
`uart_print_threadsafe()` to make your code more readable.

```c
static sched_mutex uart_mutex = SCHED_MUTEX_INIT;
int uart_print_threadsafe(char *strng) {
    sched_mutex_lock(&uart_mutex);
    int err = sched_syscall(uart_print_syscall, strng);
    sched_mutex_unlock(&uart_mutex);
    return err;
}
```
