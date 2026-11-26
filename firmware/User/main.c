#include "stm32f10x.h"
#include "stdlib.h"
#include "stdio.h"
#include "stdbool.h"
#include "delay.h"
#include "usart.h"
#include "timer.h"
#include "bkp_state.h"
#include "usb_cdc.h"
#include "max30102.h"
#include "oled.h"
#include "ds18b20.h"
#include "led.h"
#include "adxl345.h"
#include "algorithm.h"
#include "alarm.h"
#include "fall.h"
#include "flash_hist.h"

/************************* MAX30102 算法缓冲与状态 *************************/
uint32_t aun_ir_buffer[500];
uint32_t aun_red_buffer[500];
int32_t  n_sp02 = 0;
int8_t   ch_spo2_valid = 0;
int32_t  n_heart_rate = 0;
int8_t   ch_hr_valid = 0;

u8 dis_hr = 0, dis_spo2 = 0;
u8 hr = 0, spo2 = 0;

static u16 max_fill_idx = 0;
static u8  max_warmed_up = 0;
static u8  max_calc_pending = 0;

/************************* 运动、温度与全局阈值 **************************/
float x_angle = 0, y_angle = 0, z_angle = 0;
u16   step_num = 0;
u8    step_count = 0;
typedef struct {
    float x[2];
    float y[2];
    float z[2];
} StepStruct;
StepStruct stepStruct;

short temperature = 0;
u8    ds18b20_ok = 0;

/* 默认报警阈值（可通过 USB CDC / USART1 修改并写入第 61 页 Flash 掉电保存） */
u8 hr_min = 50;
u8 hr_max = 120;
u8 spo2_min = 90;
u8 temperature_min = 20;
u8 temperature_max = 40;

static bool OLED_state = false;
static u8   sos_drawn = 0;

/* 中断到主循环的轻量事件标志（避免中断内调用 I2C / SysTick / Flash） */
volatile u8 fall_tick_50ms_flag = 0;
static volatile u8 tick_500ms_flag = 0;
static volatile u8 tick_ui_flag = 0;
static volatile u8 push_1s_flag = 0;
static volatile u8 bkp_save_flags_flag = 0;
static volatile u8 bkp_save_all_flag = 0;
static volatile u8 auto_save_flag = 0;
static volatile u8 hist_next_auto_flag = 0;

static volatile u8 dbl_win_ticks = 0;
static volatile u8 sec_half = 0;
static volatile u8 hist_carousel_sec = 0;

static void Sync_State_To_BKP(u8 full)
{
    sys_state.hr           = hr;
    sys_state.spo2         = spo2;
    sys_state.step_num     = step_num;
    sys_state.temperature  = temperature;
    sys_state.alarm_active = Alarm_IsActive();
    sys_state.alarm_ack    = Alarm_IsAcked();
    sys_state.sos_active   = Fall_IsSOSActive();
    sys_state.manual_sos   = Fall_IsManualSOS();
    if (full) BKP_State_Save();
    else      BKP_State_SaveFlagsOnly();
}

void App_SetPage(u8 page)
{
    if (page >= PAGE_COUNT) page = 0;
    sys_state.page = page;
    sys_state.dbl_win = 0;
    OLED_state = true;
    hist_carousel_sec = 0;
    Sync_State_To_BKP(1);
}

void App_AckAll(void)
{
    if (Fall_IsSOSActive()) Fall_Ack();
    if (Alarm_IsActive())   Alarm_Ack();
    sys_state.sos_active = 0;
    sys_state.manual_sos = 0;
    sys_state.alarm_ack  = Alarm_IsAcked();
    sys_state.page = 0;
    OLED_state = true;
    sos_drawn = 0;
    Sync_State_To_BKP(1);
}

void App_TriggerManualSOS(void)
{
    Fall_SOS_Trigger();
    sys_state.sos_active = 1;
    sys_state.manual_sos = 1;
    if (sys_state.day_falls < 15) sys_state.day_falls++;
    Sync_State_To_BKP(1);
    Comm_SendString("EVENT,SOS=MANUAL\r\n");
}

void App_SaveHistoryNow(void)
{
    u16 time_tag;
    u8  hr_avg, hr_mx, hr_mn, sp_mn;
    u16 t_max;

    if (sys_state.time_synced)
        time_tag = (u16)(0x8000U | ((sys_state.clock_tod_sec / 60UL) % 1440UL));
    else
        time_tag = (u16)(sys_state.uptime_min & 0x7FFFU);

    hr_avg = (sys_state.day_hr_count > 0) ? (u8)(sys_state.day_hr_sum / sys_state.day_hr_count) : hr;
    hr_mx  = (sys_state.day_hr_max > 0) ? sys_state.day_hr_max : hr;
    hr_mn  = (sys_state.day_hr_min < 255) ? sys_state.day_hr_min : hr;
    sp_mn  = (sys_state.day_spo2_min < 255) ? sys_state.day_spo2_min : spo2;
    t_max  = (sys_state.day_temp_max_x10 > 0) ? (u16)sys_state.day_temp_max_x10
                                              : ((temperature > 0) ? (u16)temperature : 0);

    if (Hist_Save(time_tag, hr_avg, hr_mx, hr_mn, sp_mn, t_max,
                  step_num, sys_state.day_falls, sys_state.day_alarms))
    {
        Hist_TriggerSavedBanner();
        if (sys_state.page == 2) OLED_state = true;
    }
}

static void Send_Telemetry_1s(void)
{
    char buf[96];
    char tbuf[12];
    char clkbuf[16];
    u8 hh, mm, ss;

    if (temperature < 0)
    {
        short v = -temperature;
        sprintf(tbuf, "-%d.%d", v / 10, v % 10);
    }
    else
    {
        sprintf(tbuf, "%d.%d", temperature / 10, temperature % 10);
    }

    if (sys_state.time_synced)
    {
        hh = (u8)((sys_state.clock_tod_sec / 3600UL) % 24UL);
        mm = (u8)((sys_state.clock_tod_sec / 60UL) % 60UL);
        ss = (u8)(sys_state.clock_tod_sec % 60UL);
        sprintf(clkbuf, "%02u:%02u:%02u", hh, mm, ss);
    }
    else
    {
        hh = (u8)((sys_state.uptime_min / 60U) % 100U);
        mm = (u8)(sys_state.uptime_min % 60U);
        ss = sys_state.uptime_sec;
        sprintf(clkbuf, "U%02u:%02u:%02u", hh, mm, ss);
    }

    sprintf(buf, "DATA,HR=%u,SPO2=%u,TEMP=%s,STEP=%u,ALM=%u,SOS=%u,PAGE=%u,T=%s\r\n",
            hr, spo2, tbuf, step_num, Alarm_GetReason(),
            Fall_IsSOSActive(), sys_state.page, clkbuf);
    Comm_SendString(buf);
}

static void Process_MAX30102_NonBlocking(void)
{
    static u8 invalid_hr_cnt = 0;
    static u8 hr_jump_cnt = 0;
    u8 fifo_reads = 0;
    u16 i;

    while (MAX30102_INT == 0 && fifo_reads < 8U)
    {
        u8 tbuf[6];
        max30102_FIFO_ReadBytes(REG_FIFO_DATA, tbuf);
        aun_red_buffer[max_fill_idx] = ((u32)(tbuf[0] & 0x03) << 16) | ((u32)tbuf[1] << 8) | tbuf[2];
        aun_ir_buffer[max_fill_idx]  = ((u32)(tbuf[3] & 0x03) << 16) | ((u32)tbuf[4] << 8) | tbuf[5];
        max_fill_idx++;
        fifo_reads++;
        if (max_fill_idx >= 500U)
        {
            max_calc_pending = 1;
            break;
        }
    }

    if (max_calc_pending)
    {
        max_calc_pending = 0;
        maxim_heart_rate_and_oxygen_saturation(
            aun_ir_buffer, 500, aun_red_buffer,
            &n_sp02, &ch_spo2_valid, &n_heart_rate, &ch_hr_valid);

        /* 心率：结合算法内无手指检出与峰间距 33 点限制，再做抗毛刺平滑与离指归零 */
        if (ch_hr_valid == 1 && n_heart_rate >= 40 && n_heart_rate <= 185)
        {
            u8 new_hr = (u8)n_heart_rate;
            invalid_hr_cnt = 0;
            if (dis_hr == 0)
            {
                dis_hr = new_hr;
                hr = dis_hr;
                hr_jump_cnt = 0;
            }
            else
            {
                u8 diff = (new_hr > dis_hr) ? (new_hr - dis_hr) : (dis_hr - new_hr);
                if (diff > 25 && hr_jump_cnt == 0)
                {
                    /* 单次跳变超过 25 bpm：先暂留等下一秒再次确认，不立刻拉高 */
                    hr_jump_cnt = 1;
                    ch_hr_valid = 0;
                }
                else
                {
                    hr_jump_cnt = 0;
                    dis_hr = (u8)(((u16)dis_hr * 2U + (u16)new_hr + 1U) / 3U);
                    hr = dis_hr;
                }
            }
        }
        else
        {
            ch_hr_valid = 0;
            hr_jump_cnt = 0;
            if (++invalid_hr_cnt >= 2)
            {
                /* 连续 2 秒未检测到有效手指脉搏，显示归零并清除虚高旧值 */
                dis_hr = 0;
                hr = 0;
                dis_spo2 = 0;
                spo2 = 0;
            }
        }

        if (ch_spo2_valid == 1 && n_sp02 >= 50 && n_sp02 <= 100 && invalid_hr_cnt == 0)
        {
            dis_spo2 = (dis_spo2 == 0) ? (u8)n_sp02 : (u8)(((u16)dis_spo2 * 2U + (u16)n_sp02 + 1U) / 3U);
            spo2 = dis_spo2;
        }
        else
        {
            ch_spo2_valid = 0;
        }

        for (i = 100U; i < 500U; i++)
        {
            aun_red_buffer[i - 100U] = aun_red_buffer[i];
            aun_ir_buffer[i - 100U]  = aun_ir_buffer[i];
        }
        max_fill_idx = 400U;
        max_warmed_up = 1;
    }
}

int main(void)
{
    BootAction boot_act;
    u8 ds_retry = 0;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    delay_init(72);

    /* 先从 BKP 识别是冷上电还是按下 RESET 键，并恢复步数/页面/体征 */
    boot_act = BKP_State_InitAndDetect();
    hr          = sys_state.hr;
    dis_hr      = sys_state.hr;
    spo2        = sys_state.spo2;
    dis_spo2    = sys_state.spo2;
    step_num    = sys_state.step_num;
    temperature = sys_state.temperature;
    dbl_win_ticks = 0;

    USART1_Config();
    USB_CDC_Init(1);
    OLED_Init();
    ADXL345_Init();
    max30102_init();
    led_init();

    ds18b20_ok = 1;
    while (DS18B20_Init())
    {
        if (++ds_retry >= 2)
        {
            ds18b20_ok = 0;
            break;
        }
        delay_ms(5);
    }

    Config_Load(&hr_min, &hr_max, &spo2_min, &temperature_min, &temperature_max);
    Hist_Init();

    Alarm_RestoreState(sys_state.alarm_active, sys_state.alarm_ack, hr, spo2, temperature);
    Fall_RestoreSOS(sys_state.sos_active, sys_state.manual_sos);

    if (boot_act == BOOT_BTN_DOUBLE)
    {
        if (sys_state.sos_active)
        {
            if (sys_state.day_falls < 15) sys_state.day_falls++;
            BKP_State_Save();
            Fall_ShowHelp();
            sos_drawn = 1;
        }
        else if (sys_state.page == 2)
        {
            App_SaveHistoryNow();
            OLED_ShowPage(2);
        }
        else
        {
            OLED_ShowPage(sys_state.page);
        }
    }
    else
    {
        if (Fall_IsSOSActive())
        {
            Fall_ShowHelp();
            sos_drawn = 1;
        }
        else
        {
            OLED_ShowPage(sys_state.page);
        }
    }

    Timer2_Init(500);       // 500ms 系统节拍（仅置标志位，无耗时操作）
    TIM3_Init(499, 7199);   // 50ms 跌倒采样节拍与 SOS LED 爆闪

    while (1)
    {
        /* 1. 串口 / USB CDC 指令轮询（任意页面随时响应） */
        Comm_Poll();

        /* 2. 50ms ADXL345 跌倒状态机（主循环执行，杜绝 I2C/SysTick 中断冲突） */
        if (fall_tick_50ms_flag)
        {
            fall_tick_50ms_flag = 0;
            Fall_Tick_50ms();
            if (Fall_HasNewEvent())
            {
                if (sys_state.day_falls < 15) sys_state.day_falls++;
                Sync_State_To_BKP(1);
                Comm_SendString(Fall_IsManualSOS() ? "EVENT,SOS=MANUAL\r\n" : "EVENT,SOS=FALL\r\n");
            }
        }

        /* 3. 非阻塞 MAX30102 心率/血氧 FIFO 采集与周期解算（所有页面持续后台运行） */
        Process_MAX30102_NonBlocking();

        /* 4. 500ms 周期任务：计步、测温、统计累计、A1 越界报警判断 */
        if (tick_500ms_flag)
        {
            float ax, ay, az;
            short t_raw;
            u8 temp_ok;
            u8 h_valid_use, s_valid_use;

            tick_500ms_flag = 0;

            if (Fall_ConsumeAvgXYZ(&ax, &ay, &az))
            {
                ADXL345_CalcAngle(ax, ay, az, &x_angle, &y_angle, &z_angle);
                if (step_count == 2)
                {
                    if ((abs((int)(stepStruct.x[1] - stepStruct.x[0])) > 30) ||
                        (abs((int)(stepStruct.y[1] - stepStruct.y[0])) > 30) ||
                        (abs((int)(stepStruct.z[1] - stepStruct.z[0])) > 30))
                    {
                        step_num++;
                    }
                    step_count = 0;
                }
                stepStruct.x[step_count] = x_angle;
                stepStruct.y[step_count] = y_angle;
                stepStruct.z[step_count] = z_angle;
                step_count++;
            }

            if (ds18b20_ok)
            {
                t_raw = DS18B20_Get_Temp();
                /* 上电前 2 秒过滤 DS18B20 尚未完成首次转换时的 85.0℃ 寄存器默认值 */
                if (!(t_raw == 850 && sys_state.uptime_min == 0 && sys_state.uptime_sec < 2))
                {
                    temperature = t_raw;
                }
            }
            temp_ok = (ds18b20_ok && temperature >= -50 && temperature <= 850 &&
                       !(temperature == 850 && sys_state.uptime_min == 0 && sys_state.uptime_sec < 2)) ? 1 : 0;

            if (max_warmed_up && ch_hr_valid && dis_hr > 0)
            {
                sys_state.day_hr_sum += dis_hr;
                sys_state.day_hr_count++;
                if (dis_hr > sys_state.day_hr_max) sys_state.day_hr_max = dis_hr;
                if (dis_hr < sys_state.day_hr_min) sys_state.day_hr_min = dis_hr;
            }
            if (max_warmed_up && ch_spo2_valid && dis_spo2 > 0 && dis_spo2 < sys_state.day_spo2_min)
            {
                sys_state.day_spo2_min = dis_spo2;
            }
            if (temp_ok && temperature > sys_state.day_temp_max_x10)
            {
                sys_state.day_temp_max_x10 = temperature;
            }

            /* 刚按 RESET 后的前 5s 预热期内若原本就在报警，维持有效判定以免闪烁解锁 */
            h_valid_use = max_warmed_up ? (u8)ch_hr_valid   : (u8)(Alarm_IsActive() && hr > 0);
            s_valid_use = max_warmed_up ? (u8)ch_spo2_valid : (u8)(Alarm_IsActive() && spo2 > 0);

            Alarm_Tick(h_valid_use, dis_hr, s_valid_use, dis_spo2,
                       temp_ok, temperature,
                       hr_min, hr_max, spo2_min, temperature_min, temperature_max);

            if (Alarm_HasNewEvent())
            {
                char ev[32];
                if (sys_state.day_alarms < 15) sys_state.day_alarms++;
                sys_state.page = 1;   // 新报警切至第 1 页报警界面
                OLED_state = true;
                Sync_State_To_BKP(1);
                sprintf(ev, "EVENT,ALM=0x%02X\r\n", Alarm_GetReason());
                Comm_SendString(ev);
            }
            else
            {
                sys_state.alarm_active = Alarm_IsActive();
                sys_state.alarm_ack    = Alarm_IsAcked();
            }
        }

        /* 5. BKP 同步、历史定时自动保存与轮播、1s 串口推流 */
        if (bkp_save_all_flag)
        {
            bkp_save_all_flag = 0;
            bkp_save_flags_flag = 0;
            Sync_State_To_BKP(1);
        }
        else if (bkp_save_flags_flag)
        {
            bkp_save_flags_flag = 0;
            Sync_State_To_BKP(0);
        }

        if (hist_next_auto_flag)
        {
            hist_next_auto_flag = 0;
            Hist_NextAuto();
        }

        if (auto_save_flag)
        {
            auto_save_flag = 0;
            App_SaveHistoryNow();
        }

        if (push_1s_flag)
        {
            push_1s_flag = 0;
            Send_Telemetry_1s();
        }

        /* 6. OLED 显示状态机（按需单次整帧刷新，零闪屏） */
        if (Fall_IsSOSActive())
        {
            if (!sos_drawn)
            {
                Fall_ShowHelp();
                sos_drawn = 1;
            }
        }
        else
        {
            if (sos_drawn)
            {
                sos_drawn = 0;
                OLED_ShowPage(sys_state.page);
            }
            if (OLED_state)
            {
                OLED_state = false;
                OLED_ShowPage(sys_state.page);
            }

            switch (sys_state.page)
            {
                case 0:
                    if (tick_ui_flag)
                    {
                        tick_ui_flag = 0;
                        OLED_DrawHomeLive();
                    }
                    break;
                case 1:
                    Alarm_Refresh();
                    break;
                case 2:
                    if (Hist_NeedRedraw()) Hist_ShowPage();
                    break;
                default:
                    sys_state.page = 0;
                    OLED_state = true;
                    break;
            }
        }
    }
}

void TIM2_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM2, TIM_IT_Update) == SET)
    {
        TIM_ClearITPendingBit(TIM2, TIM_IT_Update);

        tick_500ms_flag = 1;
        tick_ui_flag = 1;

        /* 1.5s RESET 双击检测窗口计时 */
        if (sys_state.dbl_win)
        {
            if (++dbl_win_ticks >= 3)
            {
                dbl_win_ticks = 0;
                sys_state.dbl_win = 0;
                sys_state.prev_page = sys_state.page;
                bkp_save_flags_flag = 1;
            }
        }

        /* 每两次 500ms 计 1 秒 */
        sec_half ^= 1U;
        if (sec_half == 0U)
        {
            if (++sys_state.uptime_sec >= 60U)
            {
                sys_state.uptime_sec = 0;
                sys_state.uptime_min++;
                if (sys_state.uptime_min > 0 && (sys_state.uptime_min % 5U) == 0U)
                {
                    auto_save_flag = 1;   // 每 5 分钟自动记录一条 Flash 历史
                }
            }
            if (sys_state.time_synced)
            {
                sys_state.clock_tod_sec = (sys_state.clock_tod_sec + 1UL) % 86400UL;
            }
            if (sys_state.page == 2)
            {
                if (++hist_carousel_sec >= 3U)
                {
                    hist_carousel_sec = 0;
                    hist_next_auto_flag = 1;
                }
            }
            else
            {
                hist_carousel_sec = 0;
            }
            bkp_save_all_flag = 1;
            push_1s_flag = 1;
        }
    }
}
