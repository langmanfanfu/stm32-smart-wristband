#include "bkp_state.h"

SysState sys_state;

void BKP_State_ResetDayStats(void)
{
    sys_state.day_hr_sum = 0;
    sys_state.day_hr_count = 0;
    sys_state.day_hr_max = 0;
    sys_state.day_hr_min = 255;
    sys_state.day_spo2_min = 255;
    sys_state.day_temp_max_x10 = -500;
    sys_state.day_falls = 0;
    sys_state.day_alarms = 0;
}

static void BKP_State_ClearAll(void)
{
    sys_state.page = 0;
    sys_state.prev_page = 0;
    sys_state.dbl_win = 0;
    sys_state.alarm_active = 0;
    sys_state.alarm_ack = 0;
    sys_state.sos_active = 0;
    sys_state.manual_sos = 0;
    sys_state.time_synced = 0;
    sys_state.hr = 0;
    sys_state.spo2 = 0;
    sys_state.step_num = 0;
    sys_state.temperature = 0;
    sys_state.uptime_min = 0;
    sys_state.uptime_sec = 0;
    sys_state.clock_tod_sec = 0;
    BKP_State_ResetDayStats();
}

static u16 BKP_PackFlags(void)
{
    u16 f = 0;
    f |= ((u16)(sys_state.page & 0x03));
    f |= ((u16)(sys_state.prev_page & 0x03)) << 2;
    if (sys_state.dbl_win)      f |= (1U << 4);
    if (sys_state.alarm_active) f |= (1U << 5);
    if (sys_state.alarm_ack)    f |= (1U << 6);
    if (sys_state.sos_active)   f |= (1U << 7);
    if (sys_state.manual_sos)   f |= (1U << 8);
    if (sys_state.time_synced)  f |= (1U << 9);
    f |= ((u16)(sys_state.uptime_sec & 0x3F)) << 10;
    return f;
}

void BKP_State_SaveFlagsOnly(void)
{
    BKP_WriteBackupRegister(BKP_DR1, BKP_STATE_MAGIC);
    BKP_WriteBackupRegister(BKP_DR2, BKP_PackFlags());
}

void BKP_State_Save(void)
{
    u16 dr9, dr10;
    u16 clk_min, clk_sec2;

    BKP_WriteBackupRegister(BKP_DR1, BKP_STATE_MAGIC);
    BKP_WriteBackupRegister(BKP_DR2, BKP_PackFlags());
    BKP_WriteBackupRegister(BKP_DR3, sys_state.step_num);
    BKP_WriteBackupRegister(BKP_DR4, ((u16)sys_state.hr << 8) | sys_state.spo2);
    BKP_WriteBackupRegister(BKP_DR5, (u16)sys_state.temperature);
    BKP_WriteBackupRegister(BKP_DR6, sys_state.uptime_min);
    BKP_WriteBackupRegister(BKP_DR7, (u16)sys_state.day_temp_max_x10);
    BKP_WriteBackupRegister(BKP_DR8, ((u16)sys_state.day_hr_max << 8) | sys_state.day_hr_min);

    dr9 = ((u16)sys_state.day_spo2_min << 8) |
          ((u16)(sys_state.day_falls & 0x0F) << 4) |
          (u16)(sys_state.day_alarms & 0x0F);
    BKP_WriteBackupRegister(BKP_DR9, dr9);

    clk_min  = (u16)((sys_state.clock_tod_sec / 60UL) % 1440UL);
    clk_sec2 = (u16)((sys_state.clock_tod_sec % 60UL) >> 1);
    dr10 = (clk_min << 5) | (clk_sec2 & 0x1F);
    BKP_WriteBackupRegister(BKP_DR10, dr10);
}

static void BKP_State_Load(void)
{
    u16 dr2, dr4, dr8, dr9, dr10;
    u16 clk_min, clk_sec2;

    dr2  = BKP_ReadBackupRegister(BKP_DR2);
    dr4  = BKP_ReadBackupRegister(BKP_DR4);
    dr8  = BKP_ReadBackupRegister(BKP_DR8);
    dr9  = BKP_ReadBackupRegister(BKP_DR9);
    dr10 = BKP_ReadBackupRegister(BKP_DR10);

    sys_state.page         = (u8)(dr2 & 0x03);
    if (sys_state.page >= PAGE_COUNT) sys_state.page = 0;
    sys_state.prev_page    = (u8)((dr2 >> 2) & 0x03);
    if (sys_state.prev_page >= PAGE_COUNT) sys_state.prev_page = 0;
    sys_state.dbl_win      = (dr2 & (1U << 4)) ? 1 : 0;
    sys_state.alarm_active = (dr2 & (1U << 5)) ? 1 : 0;
    sys_state.alarm_ack    = (dr2 & (1U << 6)) ? 1 : 0;
    sys_state.sos_active   = (dr2 & (1U << 7)) ? 1 : 0;
    sys_state.manual_sos   = (dr2 & (1U << 8)) ? 1 : 0;
    sys_state.time_synced  = (dr2 & (1U << 9)) ? 1 : 0;
    sys_state.uptime_sec   = (u8)((dr2 >> 10) & 0x3F);
    if (sys_state.uptime_sec >= 60) sys_state.uptime_sec = 0;

    sys_state.step_num         = BKP_ReadBackupRegister(BKP_DR3);
    sys_state.hr               = (u8)(dr4 >> 8);
    sys_state.spo2             = (u8)(dr4 & 0xFF);
    sys_state.temperature      = (short)BKP_ReadBackupRegister(BKP_DR5);
    sys_state.uptime_min       = BKP_ReadBackupRegister(BKP_DR6);
    sys_state.day_temp_max_x10 = (short)BKP_ReadBackupRegister(BKP_DR7);
    sys_state.day_hr_max       = (u8)(dr8 >> 8);
    sys_state.day_hr_min       = (u8)(dr8 & 0xFF);
    sys_state.day_spo2_min     = (u8)(dr9 >> 8);
    sys_state.day_falls        = (u8)((dr9 >> 4) & 0x0F);
    sys_state.day_alarms       = (u8)(dr9 & 0x0F);

    clk_min  = (dr10 >> 5) & 0x07FF;
    clk_sec2 = dr10 & 0x1F;
    if (clk_min < 1440 && clk_sec2 < 30)
        sys_state.clock_tod_sec = (u32)clk_min * 60UL + (u32)clk_sec2 * 2UL;
    else
        sys_state.clock_tod_sec = 0;

    if (sys_state.day_hr_max > 0 && sys_state.day_hr_min < 255)
    {
        sys_state.day_hr_sum   = (u32)(((u16)sys_state.day_hr_max + sys_state.day_hr_min) / 2);
        sys_state.day_hr_count = 1;
    }
    else
    {
        sys_state.day_hr_sum   = 0;
        sys_state.day_hr_count = 0;
    }
}

BootAction BKP_State_InitAndDetect(void)
{
    FlagStatus por, pin, sft;
    u8 magic_ok;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);
    PWR_BackupAccessCmd(ENABLE);

    por = RCC_GetFlagStatus(RCC_FLAG_PORRST);
    pin = RCC_GetFlagStatus(RCC_FLAG_PINRST);
    sft = RCC_GetFlagStatus(RCC_FLAG_SFTRST);
    magic_ok = (BKP_ReadBackupRegister(BKP_DR1) == BKP_STATE_MAGIC) ? 1 : 0;
    RCC_ClearFlag();

    if (por != RESET || !magic_ok)
    {
        BKP_State_ClearAll();
        BKP_State_Save();
        return BOOT_COLD_POWERON;
    }

    BKP_State_Load();

    if (sft != RESET || pin == RESET)
    {
        // 仅软件复位/调试器复位，不触发按键动作
        sys_state.dbl_win = 0;
        BKP_State_Save();
        return BOOT_COLD_POWERON;
    }

    // 物理按下核心板 RESET 键 (PINRST)
    if (sys_state.sos_active || (sys_state.alarm_active && !sys_state.alarm_ack))
    {
        // 报警或求救状态下单按 RESET：确认消警并返回主页
        sys_state.sos_active = 0;
        sys_state.manual_sos = 0;
        if (sys_state.alarm_active) sys_state.alarm_ack = 1;
        sys_state.dbl_win = 0;
        sys_state.page = 0;
        BKP_State_Save();
        return BOOT_BTN_SINGLE;
    }

    if (sys_state.dbl_win)
    {
        // 1.5s 内双击 RESET
        sys_state.dbl_win = 0;
        if (sys_state.prev_page == 2)
        {
            // 历史页双击：留在历史页并保存一条快照
            sys_state.page = 2;
        }
        else
        {
            // 其他页双击：触发手动 SOS
            sys_state.page = 0;
            sys_state.sos_active = 1;
            sys_state.manual_sos = 1;
        }
        BKP_State_Save();
        return BOOT_BTN_DOUBLE;
    }

    // 首次单按：切换下一页并开启 1.5s 双击窗口
    sys_state.prev_page = sys_state.page;
    sys_state.page = (u8)((sys_state.page + 1) % PAGE_COUNT);
    sys_state.dbl_win = 1;
    BKP_State_Save();
    return BOOT_BTN_SINGLE;
}
