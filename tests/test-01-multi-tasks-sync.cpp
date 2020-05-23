
#ifdef __SCHEDULER_TEST1__

#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/cm3/nvic.h>

#include <perip/uart/IntBufUart.hpp>
#include <system/LED.hpp>
#include <system/time/MicroTimer.hpp>

#include <system/scheduler/scheduler.h>

// USART1 
IntBufUart uart;
static char uart_tx_buf[1024];
static char uart_rx_buf[32];

extern "C" void USART1_IRQHandler() {
	uart.handle_interrupt();
}

void uart_init() {
    uart.init_tx_buffer(uart_tx_buf, sizeof(uart_tx_buf));
    uart.init_rx_buffer(uart_rx_buf, sizeof(uart_rx_buf));
    uart.init(USART1, GPIOA, GPIO_USART1_TX | GPIO_USART1_RX, 921600);
}

#define NUM_TASKS 32

// Scheduler
sched_task tasks[NUM_TASKS+1];
uint8_t __attribute__((aligned(8))) stacks[NUM_TASKS+1][128];
uint32_t goals[NUM_TASKS+1] = {0};

sched_mutex mutex = SCHED_MUTEX_INIT;
sched_cond cond = SCHED_COND_INIT;


extern "C" uint8_t rand8();

extern "C" void sched_expect_fail(char *file, int lineno) {
    uart.print("\r\nSCHED expect() fail in ");
    uart.print(file);
    uart.print(" at line ");
    uart.print((uint32_t) lineno);
    uart.print("\r\n");
    while(1);
}

// Consumer task #(2n)
void enter1(void *arg) {
    while(1) {
        // Acquire mutex and wait for conditional variable
        sched_mutex_lock(&mutex);
        sched_cond_wait(&cond, &mutex);
        // Let the user know that we are now using the CPU
        uint32_t index = (uint32_t) arg;
        uart.putchar('A' + ((char) index));
        // Simulate CPU usage
        uint16_t delay =  (((uint16_t) rand8() << 8) | rand8()) & 0xff;
        MicroTimer::delay(delay);
        // Finally unlock the mutex and let other tasks in
        sched_mutex_unlock(&mutex);

        // Let other tasks work
        uint16_t period = (((uint16_t) rand8() << 8) | rand8()) & 0x3fff;
        sched_task_sleepuntil(goals[index] += period);
    }
}

// Consumer task #(2n+1)
void enter2(void *arg) {
    while(1) {
        // Acquire mutex and wait for conditional variable
        sched_mutex_lock(&mutex);
        sched_cond_wait(&cond, &mutex);
        // Let the user know that we are now using the CPU
        uint32_t index = (uint32_t) arg;
        uart.putchar('a' + ((char) index));
        // Simulate CPU usage
        uint16_t delay =  (((uint16_t) rand8() << 8) | rand8()) & 0xff;
        MicroTimer::delay(delay);
        // Finally unlock the mutex and let other tasks in
        sched_mutex_unlock(&mutex);

        // Let other tasks work
        uint16_t period = (((uint16_t) rand8() << 8) | rand8()) & 0x3fff;
        sched_task_sleepuntil(goals[index] += period);
    }
}

// Signaler task #NUM_TASKS
void enter3(void *arg) {
    while(1) {
        // Acquire mutex
        sched_mutex_lock(&mutex);
        // Let the user know that we are now using the CPU
        uart.putchar('-');
        // Signalize via ocnditional variable
        sched_cond_broadcast(&cond);
        // Leave mutex to let other tasks in
        sched_mutex_unlock(&mutex);

        // Wait and repeat
        sched_task_sleepuntil(goals[NUM_TASKS] += 0x800);
    }
}


void sched_doinit() {
    sched_init();
    for(int i = 0; i < 32; i++)
        rand8();
    // Display info about sleep task
    uart.print("0th task [");
    uart.print((uint32_t) &scheduler.sleep_task, 16);
    uart.print("]\r\n");

    // Initialize and add consumer tasks with random period >= 0x800
    for(unsigned i = 0; i < NUM_TASKS; i++) {
        uint8_t *stack = stacks[i];
        sched_task *task = &tasks[i];
        sched_task_init(task, 0x7f, stack, 128, (i & 1) ? enter2 : enter1, (void*) (uint32_t) i);
        uint16_t period ;
        do {
            period =  (((uint16_t) rand8() << 8) | rand8()) & 0x7fff;
        } while(period < 0x800);
        uart.print((uint32_t) i);
        uart.print("th task (");
        uart.putchar('A' + (uint8_t) i);
        uart.print(")\r\n");
        sched_task_add(task);
    }

    // Initialize and add signaler task with period 0x800
    uint8_t *stack = stacks[NUM_TASKS];
    sched_task *task = &tasks[NUM_TASKS];
    sched_task_init(task, 0x7f, stack, 128, enter3, NULL);
    sched_task_add(task);
}



extern "C" int program() {
    // Init UART
    uart_init();
    uart.print("Initializing scheduler!\r\n");

    // Init scheduler
    sched_doinit();
    uart.print("Initialized, branching!\r\n");
    uart.join();

    sched_start();
    __builtin_unreachable();
}


void loop() {
    while(1) {
        MicroTimer::delay(50e3);
        LED::toggle();
    }
}

extern "C" void HardFault_Handler() {
    // Show where HardFault happened
    char *str = (char*) "<<<HardFault>>>";
    while(*str) {
        usart_send_blocking(uart.UARTx, (uint8_t) *str);
        str++;
    }
    // Empty the uart buffer
    while(true) {
        int ch = uart.pop();
        if(ch == -1) break;
        usart_send_blocking(uart.UARTx, (uint8_t) ch);
    }
    // Infinitely loo
    loop();
}

#endif
