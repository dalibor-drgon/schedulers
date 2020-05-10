
#ifndef __SCHEDULER_CONFIG_H
#define __SCHEDULER_CONFIG_H

/// Low 16bits will be counter by this timer.
/// TODO: Change this if needed
#define SCHED_TIMlo TIM3

/// SCHED_TIMlo 's IRQHandler.
/// TODO: Change this if needed
#define SCHED_TIMlo_IRQHandler TIM3_IRQHandler

/// High 16bits will be counted by this timer.
/// TODO: Change this if needed
#define SCHED_TIMhi TIM4

/// SCHED_TIMhi 's IRQHandler.
/// TODO: Change this if needed
#define SCHED_TIMhi_IRQHandler TIM4_IRQHandler

/// Internal trigger connection: TIMhi as slave, TIMlo as master.
/// TODO: Find out the value from datasheet
#define SCHED_ITR 0b010

/// CPU frequency in MHz.
/// TODO: Replace this with the correct CPU frequency in MHz
#define SCHED_CPU_MHZ 72

#endif
