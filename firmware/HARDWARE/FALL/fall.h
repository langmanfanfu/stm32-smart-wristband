#ifndef __FALL_H
#define __FALL_H
#include "stm32f10x.h"

// A2 跌倒检测 + SOS（已适配单 RESET 按键 + PC13 快闪视觉求救）
void Fall_Tick_50ms(void);
void Fall_SOS_LED_Tick_50ms(void);
u8   Fall_ConsumeAvgXYZ(float *ax, float *ay, float *az);
void Fall_SOS_Trigger(void);
void Fall_RestoreSOS(u8 active, u8 manual);
void Fall_Ack(void);
u8   Fall_IsSOSActive(void);
u8   Fall_HasNewEvent(void);
u8   Fall_IsManualSOS(void);
void Fall_ShowHelp(void);

#endif
