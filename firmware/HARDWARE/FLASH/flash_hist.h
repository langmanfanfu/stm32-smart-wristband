#ifndef __FLASH_HIST_H
#define __FLASH_HIST_H
#include "stm32f10x.h"

// A6 历史记录 + 掉电阈值配置：
//   第 61 页 (0x0800F400): 阈值配置区（USB/串口改阈值后掉电保留）
//   第 62/63 页 (0x0800F800~0x0800FFFF): 历史记录区，最多 128 条（16B/条）
#define HIST_MAX_RECORDS  128

typedef struct
{
    u16 time_tag;     // bit15=1: 对时 HH*60+MM; bit15=0: 运行分钟 uptime_min
    u16 seq;          // 记录序号 #1..65535
    u8  hr_avg;
    u8  hr_max;
    u8  hr_min;
    u8  spo2_min;
    u16 temp_max_x10; // 单位 0.1℃
    u16 steps;
    u8  falls;
    u8  alarms;
} HistRecord;

void Config_Load(u8 *hr_min_p, u8 *hr_max_p, u8 *spo2_min_p, u8 *t_min_p, u8 *t_max_p);
u8   Config_Save(u8 hr_min_v, u8 hr_max_v, u8 spo2_min_v, u8 t_min_v, u8 t_max_v);

void Hist_Init(void);
u8   Hist_Count(void);
u8   Hist_Save(u16 time_tag, u8 hr_avg, u8 hr_max, u8 hr_min,
               u8 spo2_min, u16 temp_max_x10, u16 steps, u8 falls, u8 alarms);
void Hist_ClearAll(void);
u8   Hist_GetRecord(u8 index_from_latest, HistRecord *rec);
void Hist_ShowPage(void);
void Hist_NextAuto(void);
void Hist_BrowseOlder(void);
void Hist_BrowseNewer(void);
void Hist_TriggerSavedBanner(void);
u8   Hist_NeedRedraw(void);

#endif
