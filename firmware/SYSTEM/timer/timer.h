#ifndef __TIMER_H
#define __TIMER_H
#include "stm32f10x.h" 

void Timer2_Init(u16 interval_ms);
void TIM3_Init(u16 arr,u16 psc);

#endif
