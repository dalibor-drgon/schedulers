
#ifndef __SCHEDULER_CONFIG_H
#define __SCHEDULER_CONFIG_H

#ifndef SCHED_TIMlo
/// Low 16bits will be counter by this timer.
/// TODO: Change this if needed
#define SCHED_TIMlo TIM3
#endif

#ifndef SCHED_TIMlo_IRQHandler
/// SCHED_TIMlo 's IRQHandler.
/// TODO: Change this if needed
#define SCHED_TIMlo_IRQHandler TIM3_IRQHandler
#endif

#ifndef SCHED_TIMhi
/// High 16bits will be counted by this timer.
/// TODO: Change this if needed
#define SCHED_TIMhi TIM4
#endif

#ifndef SCHED_TIMhi_IRQHandler
/// SCHED_TIMhi 's IRQHandler.
/// TODO: Change this if needed
#define SCHED_TIMhi_IRQHandler TIM4_IRQHandler
#endif

#ifndef SCHED_ITR
/// Internal trigger connection: TIMhi as slave, TIMlo as master.
/// TODO: Find out the value from datasheet
#define SCHED_ITR 0b010
#endif

#ifndef SCHED_CPU_MHZ
/// CPU frequency in MHz.
/// TODO: Replace this with the correct CPU frequency in MHz
#define SCHED_CPU_MHZ 72
#endif

#endif
