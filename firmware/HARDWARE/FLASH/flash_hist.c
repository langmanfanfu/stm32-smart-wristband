#include "flash_hist.h"
#include "oled.h"
#include <stdio.h>

#define CFG_PAGE_ADDR    0x0800F400   // 第 61 页：掉电阈值配置
#define CFG_MAGIC        0xC751

#define HIST_PAGE0_ADDR  0x0800F800   // 第 62 页
#define HIST_PAGE1_ADDR  0x0800FC00   // 第 63 页
#define HIST_MAGIC       0xA6B2       // 新结构体魔数

static u8  hist_count = 0;
static u16 last_seq = 0;
static u8  browse_index = 0;
static u8  need_redraw = 0;
static u8  saved_banner_ticks = 0;

void Config_Load(u8 *hr_min_p, u8 *hr_max_p, u8 *spo2_min_p, u8 *t_min_p, u8 *t_max_p)
{
    if (*(u16 *)CFG_PAGE_ADDR == CFG_MAGIC)
    {
        u16 w1 = *(u16 *)(CFG_PAGE_ADDR + 2);
        u16 w2 = *(u16 *)(CFG_PAGE_ADDR + 4);
        u16 w3 = *(u16 *)(CFG_PAGE_ADDR + 6);
        u8 hmin = (u8)(w1 >> 8);
        u8 hmax = (u8)(w1 & 0xFF);
        u8 smin = (u8)(w2 >> 8);
        u8 tmin = (u8)(w2 & 0xFF);
        u8 tmax = (u8)(w3 & 0xFF);
        if (hmin >= 20 && hmax <= 220 && hmin + 5 <= hmax &&
            smin >= 50 && smin <= 100 &&
            tmin <= 50 && tmax <= 60 && tmin + 2 <= tmax)
        {
            *hr_min_p   = hmin;
            *hr_max_p   = hmax;
            *spo2_min_p = smin;
            *t_min_p    = tmin;
            *t_max_p    = tmax;
        }
    }
}

u8 Config_Save(u8 hr_min_v, u8 hr_max_v, u8 spo2_min_v, u8 t_min_v, u8 t_max_v)
{
    u16 w[4];
    u8 i;

    w[0] = CFG_MAGIC;
    w[1] = ((u16)hr_min_v << 8) | hr_max_v;
    w[2] = ((u16)spo2_min_v << 8) | t_min_v;
    w[3] = (u16)t_max_v;

    __disable_irq();
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    FLASH_ErasePage(CFG_PAGE_ADDR);
    for (i = 0; i < 4; i++)
    {
        if (FLASH_ProgramHalfWord(CFG_PAGE_ADDR + (u32)i * 2U, w[i]) != FLASH_COMPLETE)
        {
            FLASH_Lock();
            __enable_irq();
            return 0;
        }
    }
    FLASH_Lock();
    __enable_irq();
    return 1;
}

static u32 Hist_RecordAddr(u8 index)
{
    if (index < 64) return HIST_PAGE0_ADDR + (u32)index * 16UL;
    else            return HIST_PAGE1_ADDR + (u32)(index - 64) * 16UL;
}

static u8 Hist_RecordValid(u32 addr)
{
    if (*(u16 *)addr != HIST_MAGIC) return 0;
    if (*(u16 *)(addr + 4) == 0U || *(u16 *)(addr + 4) == 0xFFFFU) return 0;
    return 1;
}

void Hist_Init(void)
{
    u8 i;
    hist_count = 0;
    last_seq = 0;
    browse_index = 0;
    saved_banner_ticks = 0;
    for (i = 0; i < HIST_MAX_RECORDS; i++)
    {
        u32 addr = Hist_RecordAddr(i);
        if (Hist_RecordValid(addr))
        {
            hist_count++;
            last_seq = *(u16 *)(addr + 4);
        }
        else
        {
            break;
        }
    }
}

u8 Hist_Count(void)
{
    return hist_count;
}

u8 Hist_Save(u16 time_tag, u8 hr_avg, u8 hr_max, u8 hr_min,
             u8 spo2_min, u16 temp_max_x10, u16 steps, u8 falls, u8 alarms)
{
    u32 addr;
    u16 data[8];
    u8 i;

    if (hist_count >= HIST_MAX_RECORDS)
    {
        __disable_irq();
        FLASH_Unlock();
        FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
        FLASH_ErasePage(HIST_PAGE0_ADDR);
        FLASH_ErasePage(HIST_PAGE1_ADDR);
        FLASH_Lock();
        __enable_irq();
        hist_count = 0;
    }
    else if (hist_count == 0 && *(u16 *)HIST_PAGE0_ADDR != 0xFFFF)
    {
        __disable_irq();
        FLASH_Unlock();
        FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
        FLASH_ErasePage(HIST_PAGE0_ADDR);
        FLASH_Lock();
        __enable_irq();
    }
    else if (hist_count == 64)
    {
        __disable_irq();
        FLASH_Unlock();
        FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
        FLASH_ErasePage(HIST_PAGE1_ADDR);
        FLASH_Lock();
        __enable_irq();
    }

    last_seq++;
    if (last_seq == 0 || last_seq == 0xFFFF) last_seq = 1;

    addr = Hist_RecordAddr(hist_count);
    data[0] = HIST_MAGIC;
    data[1] = time_tag;
    data[2] = last_seq;
    data[3] = ((u16)hr_avg << 8) | hr_max;
    data[4] = ((u16)hr_min << 8) | spo2_min;
    data[5] = temp_max_x10;
    data[6] = steps;
    data[7] = ((u16)falls << 8) | alarms;

    __disable_irq();
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    for (i = 0; i < 8; i++)
    {
        if (FLASH_ProgramHalfWord(addr + (u32)i * 2UL, data[i]) != FLASH_COMPLETE)
        {
            FLASH_Lock();
            __enable_irq();
            return 0;
        }
    }
    FLASH_Lock();
    __enable_irq();

    hist_count++;
    browse_index = 0;
    need_redraw = 1;
    return 1;
}

void Hist_ClearAll(void)
{
    __disable_irq();
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    FLASH_ErasePage(HIST_PAGE0_ADDR);
    FLASH_ErasePage(HIST_PAGE1_ADDR);
    FLASH_Lock();
    __enable_irq();
    hist_count = 0;
    last_seq = 0;
    browse_index = 0;
    saved_banner_ticks = 0;
    need_redraw = 1;
}

u8 Hist_GetRecord(u8 index_from_latest, HistRecord *rec)
{
    u32 addr;
    u8 idx;

    if (index_from_latest >= hist_count) return 0;
    idx = (u8)(hist_count - 1U - index_from_latest);
    addr = Hist_RecordAddr(idx);
    if (!Hist_RecordValid(addr)) return 0;

    rec->time_tag     = *(u16 *)(addr + 2);
    rec->seq          = *(u16 *)(addr + 4);
    rec->hr_avg       = (u8)(*(u16 *)(addr + 6) >> 8);
    rec->hr_max       = (u8)(*(u16 *)(addr + 6) & 0xFF);
    rec->hr_min       = (u8)(*(u16 *)(addr + 8) >> 8);
    rec->spo2_min     = (u8)(*(u16 *)(addr + 8) & 0xFF);
    rec->temp_max_x10 = *(u16 *)(addr + 10);
    rec->steps        = *(u16 *)(addr + 12);
    rec->falls        = (u8)(*(u16 *)(addr + 14) >> 8);
    rec->alarms       = (u8)(*(u16 *)(addr + 14) & 0xFF);
    return 1;
}

void Hist_TriggerSavedBanner(void)
{
    saved_banner_ticks = 2;
    need_redraw = 1;
}

void Hist_ShowPage(void)
{
    HistRecord rec;
    char buf[40];

    need_redraw = 0;
    OLED_BeginUpdate();

    if (hist_count == 0)
    {
        OLED_ShowCNLine(0,  "历史记录  无数据");
        OLED_ShowCNLine(16, "双击刷新键:保存");
        OLED_ShowCNLine(32, "每5分钟自动保存");
        OLED_ShowCNLine(48, "串口SAVE也可存");
        OLED_EndUpdate();
        return;
    }

    if (!Hist_GetRecord(browse_index, &rec))
    {
        OLED_ShowCNLine(0,  "历史读取失败");
        OLED_ShowCNLine(16, "");
        OLED_ShowCNLine(32, "");
        OLED_ShowCNLine(48, "");
        OLED_EndUpdate();
        return;
    }

    if (saved_banner_ticks > 0)
    {
        saved_banner_ticks--;
        sprintf(buf, "已保存!  共%u条", hist_count);
    }
    else if (hist_count < 10)
    {
        sprintf(buf, "历史记录 第%u/%u条", browse_index + 1, hist_count);
    }
    else
    {
        sprintf(buf, "历史 第%u/%u条", browse_index + 1, hist_count);
    }
    OLED_ShowCNLine(0, buf);

    sprintf(buf, "心率:均%-3u高%-3u", rec.hr_avg, rec.hr_max);
    OLED_ShowCNLine(16, buf);

    sprintf(buf, "血氧%u%% 温%2u.%uC", rec.spo2_min,
            (unsigned)(rec.temp_max_x10 / 10U), (unsigned)(rec.temp_max_x10 % 10U));
    OLED_ShowCNLine(32, buf);

    sprintf(buf, "步数:%-4u 警%u跌%u", rec.steps, rec.alarms, rec.falls);
    OLED_ShowCNLine(48, buf);

    OLED_EndUpdate();
    need_redraw = 0;
}

void Hist_NextAuto(void)
{
    if (hist_count <= 1) return;
    browse_index = (u8)((browse_index + 1U) % hist_count);
    need_redraw = 1;
}

void Hist_BrowseOlder(void)
{
    if (hist_count == 0) return;
    if (browse_index < hist_count - 1)
    {
        browse_index++;
        need_redraw = 1;
    }
}

void Hist_BrowseNewer(void)
{
    if (browse_index > 0)
    {
        browse_index--;
        need_redraw = 1;
    }
}

u8 Hist_NeedRedraw(void)
{
    u8 r = need_redraw;
    need_redraw = 0;
    return r;
}
