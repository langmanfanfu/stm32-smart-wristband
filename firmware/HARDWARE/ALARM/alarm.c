#include "alarm.h"
#include "fall.h"
#include "led.h"
#include "oled.h"
#include <stdio.h>

extern u8 hr_min, hr_max, spo2_min, temperature_min, temperature_max;

// A1 越界报警（纯视觉 LED + OLED 提示，无声音外设）
static u8    alarm_active = 0;
static u8    alarm_reason = 0;
static u8    alarm_ack    = 0;
static u8    alarm_phase  = 0;
static u8    hold_count   = 0;
static u8    clear_count  = 0;
static u8    new_event    = 0;
static u8    last_hr = 0, last_spo2 = 0;
static short last_temp_x10 = 0;

static u8    shown_active = 0xFF, shown_reason = 0xFF, shown_ack = 0xFF;
static u8    shown_hr = 0xFF, shown_spo2 = 0xFF;
static short shown_temp = 0x7FFF;

void Alarm_RestoreState(u8 active, u8 ack, u8 hr_val, u8 spo2_val, short temp_x10)
{
    alarm_active  = active;
    alarm_ack     = ack;
    last_hr       = hr_val;
    last_spo2     = spo2_val;
    last_temp_x10 = temp_x10;
    shown_active  = 0xFF;
    if (!alarm_active || alarm_ack)
    {
        if (!Fall_IsSOSActive()) led_off();
    }
}

void Alarm_Tick(u8 hr_valid, u8 hr, u8 spo2_valid, u8 spo2,
                u8 temp_ok, short temp_x10,
                u8 min_hr, u8 max_hr, u8 min_spo2, u8 min_temp, u8 max_temp)
{
    u8 reason = 0;

    if (hr_valid && hr > 0)
    {
        if (hr > max_hr)      reason |= ALARM_HR_HIGH;
        else if (hr < min_hr) reason |= ALARM_HR_LOW;
    }
    if (spo2_valid && spo2 > 0 && spo2 < min_spo2)
    {
        reason |= ALARM_SPO2_LOW;
    }
    if (temp_ok)
    {
        if (temp_x10 > (short)max_temp * 10)      reason |= ALARM_TEMP_HIGH;
        else if (temp_x10 < (short)min_temp * 10) reason |= ALARM_TEMP_LOW;
    }

    last_hr = hr;
    last_spo2 = spo2;
    last_temp_x10 = temp_x10;

    if (reason != 0)
    {
        clear_count = 0;
        if (!alarm_active)
        {
            /* 连续 4 个节拍（2 秒）稳定越界才正式报警，防止手指刚放上瞬态尖峰误触 */
            if (++hold_count >= 4)
            {
                hold_count = 0;
                alarm_active = 1;
                alarm_ack = 0;
                new_event = 1;
                alarm_reason = reason;
            }
        }
        else
        {
            alarm_reason = reason;
        }
    }
    else
    {
        hold_count = 0;
        if (alarm_active)
        {
            if (++clear_count >= 2)
            {
                alarm_active = 0;
                alarm_reason = 0;
                alarm_ack = 0;
                clear_count = 0;
                if (!Fall_IsSOSActive()) led_off();
            }
        }
    }

    /* 非 SOS 状态下，若未确认消警则 500ms 闪烁 PC13 LED 提示 */
    if (!Fall_IsSOSActive())
    {
        if (alarm_active && !alarm_ack)
        {
            alarm_phase = !alarm_phase;
            if (alarm_phase) led_on();
            else             led_off();
        }
        else
        {
            alarm_phase = 0;
            led_off();
        }
    }
}

void Alarm_Ack(void)
{
    if (alarm_active) alarm_ack = 1;
    if (!Fall_IsSOSActive()) led_off();
}

u8 Alarm_IsActive(void)
{
    return alarm_active;
}

u8 Alarm_IsAcked(void)
{
    return alarm_ack;
}

u8 Alarm_GetReason(void)
{
    return alarm_reason;
}

u8 Alarm_HasNewEvent(void)
{
    u8 e = new_event;
    new_event = 0;
    return e;
}

static void fmt_temp(char *out, short tx10)
{
    if (tx10 < 0)
    {
        short v = -tx10;
        sprintf(out, "-%d.%dC", v / 10, v % 10);
    }
    else
    {
        sprintf(out, "%d.%dC", tx10 / 10, tx10 % 10);
    }
}

void Alarm_ShowPage(void)
{
    char buf[40], tstr[12];

    fmt_temp(tstr, last_temp_x10);

    OLED_BeginUpdate();

    if (alarm_active)
    {
        OLED_ShowCNLine(0, alarm_ack ? "异常报警  已确认" : "异常报警!");
        if      (alarm_reason & ALARM_HR_HIGH)   OLED_ShowCNLine(16, "原因:心率过高!");
        else if (alarm_reason & ALARM_HR_LOW)    OLED_ShowCNLine(16, "原因:心率过低!");
        else if (alarm_reason & ALARM_SPO2_LOW)  OLED_ShowCNLine(16, "原因:血氧过低!");
        else if (alarm_reason & ALARM_TEMP_HIGH) OLED_ShowCNLine(16, "原因:体温过高!");
        else if (alarm_reason & ALARM_TEMP_LOW)  OLED_ShowCNLine(16, "原因:体温过低!");

        sprintf(buf, "心:%-3u 血氧:%u%%", last_hr, last_spo2);
        OLED_ShowCNLine(32, buf);

        if (alarm_ack)
            sprintf(buf, "%-6s按刷新翻页", tstr);
        else
            sprintf(buf, "%-6s按刷新确认", tstr);
        OLED_ShowCNLine(48, buf);
    }
    else
    {
        OLED_ShowCNLine(0, "阈值设置    正常");

        sprintf(buf, "心:%u-%u 氧>%u%%", hr_min, hr_max, spo2_min);
        OLED_ShowCNLine(16, buf);

        sprintf(buf, "体温%u-%u %-6s", temperature_min, temperature_max, tstr);
        OLED_ShowCNLine(32, buf);

        OLED_ShowCNLine(48, "双击SOS 单按翻页");
    }

    OLED_EndUpdate();

    shown_active = alarm_active;
    shown_reason = alarm_reason;
    shown_ack    = alarm_ack;
    shown_hr     = last_hr;
    shown_spo2   = last_spo2;
    shown_temp   = last_temp_x10;
}

void Alarm_Refresh(void)
{
    if (shown_active != alarm_active || shown_reason != alarm_reason ||
        shown_ack    != alarm_ack    || shown_hr     != last_hr      ||
        shown_spo2   != last_spo2    || shown_temp   != last_temp_x10)
    {
        Alarm_ShowPage();
    }
}
