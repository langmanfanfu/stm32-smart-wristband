#ifndef __ALARM_H
#define __ALARM_H
#include "stm32f10x.h"

// 报警原因位（可多位同时置位）
#define ALARM_HR_HIGH    0x01   // 心率高于上限
#define ALARM_HR_LOW     0x02   // 心率低于下限
#define ALARM_SPO2_LOW   0x04   // 血氧低于下限
#define ALARM_TEMP_HIGH  0x08   // 体温高于上限
#define ALARM_TEMP_LOW   0x10   // 体温低于下限

void Alarm_RestoreState(u8 active, u8 ack, u8 hr_val, u8 spo2_val, short temp_x10);
void Alarm_Tick(u8 hr_valid, u8 hr, u8 spo2_valid, u8 spo2,
                u8 temp_ok, short temp_x10,
                u8 hr_min, u8 hr_max, u8 spo2_min, u8 temp_min, u8 temp_max);

void Alarm_Ack(void);
u8   Alarm_IsActive(void);
u8   Alarm_IsAcked(void);
u8   Alarm_GetReason(void);
void Alarm_ShowPage(void);
void Alarm_Refresh(void);
u8   Alarm_HasNewEvent(void);

#endif
