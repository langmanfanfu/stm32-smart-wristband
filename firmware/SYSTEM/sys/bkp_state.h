#ifndef __BKP_STATE_H
#define __BKP_STATE_H

#include "stm32f10x.h"

#define BKP_STATE_MAGIC  0xA55B
#define PAGE_COUNT       3

typedef enum {
    BOOT_COLD_POWERON = 0,  // 冷上电或 BKP 无效
    BOOT_BTN_SINGLE   = 1,  // 单按 RESET 按钮
    BOOT_BTN_DOUBLE   = 2   // 1.5s 内双击 RESET 按钮
} BootAction;

typedef struct {
    u8    page;             // 当前页面：0 主页, 1 状态/阈值/报警, 2 Flash历史
    u8    prev_page;        // 进入双击窗口前的页面
    u8    dbl_win;          // 1 = 正处于 1.5s 连按窗口
    u8    alarm_active;     // 报警状态
    u8    alarm_ack;        // 报警已确认标志（暂停 LED 闪烁）
    u8    sos_active;       // SOS/跌倒求救激活
    u8    manual_sos;       // 1=手动SOS, 0=跌倒判定
    u8    time_synced;      // 1=已通过 PC 对时
    u8    hr;               // 最近有效心率
    u8    spo2;             // 最近有效血氧
    u16   step_num;         // 步数
    short temperature;      // 体温 (0.1℃)
    u16   uptime_min;       // 累计运行分钟
    u8    uptime_sec;       // 累计运行秒 (0..59)
    u32   clock_tod_sec;    // 对时后的当日秒数 (0..86399)
    u32   day_hr_sum;       // 统计心率总和
    u16   day_hr_count;     // 统计心率次数
    u8    day_hr_max;
    u8    day_hr_min;
    u8    day_spo2_min;
    short day_temp_max_x10;
    u8    day_falls;
    u8    day_alarms;
} SysState;

extern SysState sys_state;

BootAction BKP_State_InitAndDetect(void);
void       BKP_State_Save(void);
void       BKP_State_SaveFlagsOnly(void);
void       BKP_State_ResetDayStats(void);

#endif
