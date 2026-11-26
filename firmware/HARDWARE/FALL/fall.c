#include "fall.h"
#include "adxl345.h"
#include "led.h"
#include "oled.h"

#define FALL_MAG2_HIGH    317000L  // (2.2g)^2, ADXL345 全分辨率约 256 LSB/g
#define FALL_MAG2_LOW     16384L   // (0.5g)^2
#define FALL_STILL_DELTA  45       // 50ms 相邻帧三轴差值之和静止门限
#define FALL_SETTLE_TICKS 40       // 40 * 50ms = 2 秒撞击后落地缓冲期
#define FALL_STILL_COUNT  80       // 80 * 50ms = 4 秒连续静止确认（适合演示）
#define FALL_MOVE_CANCEL  2        // 缓冲期后连续 2 帧(100ms)活动则取消

#define FALL_NORMAL       0
#define FALL_IMPACT_WAIT  1
#define FALL_SOS          2

static u8    fall_state = FALL_NORMAL;
static u8    sos_active = 0;
static u8    manual_sos = 0;
static u8    new_event  = 0;
static u16   still_count = 0;
static u16   wait_ticks = 0;
static u8    move_debounce = 0;
static short prev_x = 0, prev_y = 0, prev_z = 0;
static u8    sos_led_cnt = 0;
static u8    sos_led_phase = 0;
static long  step_sum_x = 0, step_sum_y = 0, step_sum_z = 0;
static u16   step_sample_cnt = 0;

static void Fall_EnterSOS(u8 manual)
{
    fall_state = FALL_SOS;
    sos_active = 1;
    manual_sos = manual;
    new_event = 1;
    sos_led_cnt = 0;
    sos_led_phase = 1;
    led_on();
}

void Fall_RestoreSOS(u8 active, u8 manual)
{
    if (active)
    {
        fall_state = FALL_SOS;
        sos_active = 1;
        manual_sos = manual;
        sos_led_cnt = 0;
        sos_led_phase = 1;
        led_on();
    }
    else
    {
        Fall_Ack();
    }
}

void Fall_SOS_LED_Tick_50ms(void)
{
    if (!sos_active) return;
    /* 求救状态下每 150ms 快闪一次 PC13 LED */
    if (++sos_led_cnt >= 3)
    {
        sos_led_cnt = 0;
        sos_led_phase = !sos_led_phase;
        if (sos_led_phase) led_on();
        else               led_off();
    }
}

u8 Fall_ConsumeAvgXYZ(float *ax, float *ay, float *az)
{
    if (step_sample_cnt == 0) return 0;
    *ax = (float)step_sum_x / (float)step_sample_cnt;
    *ay = (float)step_sum_y / (float)step_sample_cnt;
    *az = (float)step_sum_z / (float)step_sample_cnt;
    step_sum_x = 0;
    step_sum_y = 0;
    step_sum_z = 0;
    step_sample_cnt = 0;
    return 1;
}

void Fall_Tick_50ms(void)
{
    short x, y, z;
    long mag2;
    int delta;

    adxl345_read_data(&x, &y, &z);
    step_sum_x += x;
    step_sum_y += y;
    step_sum_z += z;
    step_sample_cnt++;

    if (fall_state == FALL_SOS)
    {
        return;
    }

    mag2 = (long)x * x + (long)y * y + (long)z * z;

    if (fall_state == FALL_NORMAL)
    {
        if (mag2 > FALL_MAG2_HIGH || mag2 < FALL_MAG2_LOW)
        {
            fall_state = FALL_IMPACT_WAIT;
            still_count = 0;
            wait_ticks = 0;
            move_debounce = 0;
        }
    }
    else
    {
        if (wait_ticks < 60000U) wait_ticks++;

        delta = 0;
        delta += (x > prev_x) ? (x - prev_x) : (prev_x - x);
        delta += (y > prev_y) ? (y - prev_y) : (prev_y - y);
        delta += (z > prev_z) ? (z - prev_z) : (prev_z - z);

        if (delta < FALL_STILL_DELTA && mag2 >= FALL_MAG2_LOW && mag2 <= FALL_MAG2_HIGH)
        {
            move_debounce = 0;
            if (++still_count >= FALL_STILL_COUNT)
            {
                Fall_EnterSOS(0);
            }
        }
        else if (wait_ticks <= FALL_SETTLE_TICKS)
        {
            /* 撞击/失重后的前 2 秒允许回弹晃动，不立即退出等待态 */
            still_count = 0;
            move_debounce = 0;
        }
        else
        {
            if (++move_debounce >= FALL_MOVE_CANCEL)
            {
                fall_state = FALL_NORMAL;
                still_count = 0;
                wait_ticks = 0;
                move_debounce = 0;
            }
        }
    }

    prev_x = x;
    prev_y = y;
    prev_z = z;
}

void Fall_SOS_Trigger(void)
{
    if (fall_state != FALL_SOS)
    {
        Fall_EnterSOS(1);
    }
}

void Fall_Ack(void)
{
    fall_state = FALL_NORMAL;
    sos_active = 0;
    manual_sos = 0;
    still_count = 0;
    wait_ticks = 0;
    move_debounce = 0;
    sos_led_cnt = 0;
    sos_led_phase = 0;
    led_off();
}

u8 Fall_IsSOSActive(void)
{
    return sos_active;
}

u8 Fall_IsManualSOS(void)
{
    return manual_sos;
}

u8 Fall_HasNewEvent(void)
{
    u8 e = new_event;
    new_event = 0;
    return e;
}

void Fall_ShowHelp(void)
{
    OLED_BeginUpdate();
    if (manual_sos)
    {
        OLED_ShowCNLine(0,  "求救!  [手动SOS]");
        OLED_ShowCNLine(16, "已手动触发求救!");
    }
    else
    {
        OLED_ShowCNLine(0,  "求救! [跌倒报警]");
        OLED_ShowCNLine(16, "检测到疑似跌倒!");
    }
    OLED_ShowCNLine(32, "按一下刷新键");
    OLED_ShowCNLine(48, "立即取消求救");
    OLED_EndUpdate();
}
