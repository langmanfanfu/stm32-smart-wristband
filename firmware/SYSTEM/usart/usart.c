#include "usart.h"
#include "usb_cdc.h"
#include "bkp_state.h"
#include "alarm.h"
#include "fall.h"
#include "flash_hist.h"
#include "oled.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UART1_RX_BUF_SIZE 96
static volatile u8  uart_rx_buf[UART1_RX_BUF_SIZE];
static volatile u16 uart_rx_head = 0, uart_rx_tail = 0;

static char cmd_line[48];
static u8   cmd_len = 0;

/* 由 main.c 提供的交互回调与配置保存 */
extern void App_SetPage(u8 page);
extern void App_SaveHistoryNow(void);
extern void App_TriggerManualSOS(void);
extern void App_AckAll(void);
extern u8 hr_min, hr_max, spo2_min, temperature_min, temperature_max;

void USART1_Config(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1 | RCC_APB2Periph_GPIOA, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    USART_InitStructure.USART_BaudRate = 115200;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART1, &USART_InitStructure);
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
    USART_Cmd(USART1, ENABLE);
}

void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
    {
        u8 ch = (u8)USART_ReceiveData(USART1);
        u16 next = (u16)((uart_rx_head + 1U) % UART1_RX_BUF_SIZE);
        if (next != uart_rx_tail)
        {
            uart_rx_buf[uart_rx_head] = ch;
            uart_rx_head = next;
        }
        USART_ClearITPendingBit(USART1, USART_IT_RXNE);
    }
}

static void USART1_SendChar(u8 ch)
{
    USART_SendData(USART1, ch);
    while (!(USART1->SR & USART_FLAG_TXE));
}

void Comm_SendString(const char *str)
{
    const char *p = str;
    if (str == 0) return;
    /* 同时发往 USB CDC 与 USART1（PA9/PA10 作备用通道） */
    USB_CDC_SendString(str);
    while (*p)
    {
        USART1_SendChar((u8)(*p++));
    }
}

int fputc(int ch, FILE *f)
{
    u8 c = (u8)ch;
    (void)f;
    USART1_SendChar(c);
    USB_CDC_Send(&c, 1);
    return ch;
}

static int ci_starts_with(const char *s, const char *prefix)
{
    while (*prefix)
    {
        char a = *s++, b = *prefix++;
        if (a >= 'a' && a <= 'z') a -= ('a' - 'A');
        if (b >= 'a' && b <= 'z') b -= ('a' - 'A');
        if (a != b) return 0;
    }
    return 1;
}

static int ci_equals(const char *a, const char *b)
{
    return ci_starts_with(a, b) && (strlen(a) == strlen(b));
}

static void Comm_HandleLine(char *line)
{
    char out[96];
    char *p = line;

    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return;

    if (ci_equals(p, "HELP") || ci_equals(p, "?"))
    {
        Comm_SendString("CMDS: PAGE 0..2 | ACK | SOS | SAVE | DUMP | CLR | TIME=HH:MM:SS | SET HR_MAX/HR_MIN/SPO2/T_MAX/T_MIN=val\r\n");
        return;
    }
    if (ci_equals(p, "ACK"))
    {
        App_AckAll();
        Comm_SendString("OK ACK\r\n");
        return;
    }
    if (ci_equals(p, "SOS"))
    {
        App_TriggerManualSOS();
        Comm_SendString("OK SOS\r\n");
        return;
    }
    if (ci_equals(p, "SAVE"))
    {
        App_SaveHistoryNow();
        Comm_SendString("OK SAVED\r\n");
        return;
    }
    if (ci_equals(p, "CLR") || ci_equals(p, "CLEAR"))
    {
        Hist_ClearAll();
        if (sys_state.page == 2) Hist_ShowPage();
        Comm_SendString("OK HIST CLEARED\r\n");
        return;
    }
    if (ci_equals(p, "DUMP"))
    {
        u8 n = Hist_Count();
        u8 i;
        HistRecord rec;
        sprintf(out, "HIST_COUNT=%u\r\n", n);
        Comm_SendString(out);
        for (i = 0; i < n; i++)
        {
            if (Hist_GetRecord(i, &rec))
            {
                u16 tm = rec.time_tag & 0x7FFFU;
                char tag_ch = (rec.time_tag & 0x8000U) ? 'T' : 'U';
                sprintf(out, "#%u,%c%02u:%02u,HRavg=%u,HRmax=%u,SPO2min=%u,Tmax=%u.%u,S=%u,F=%u,A=%u\r\n",
                        rec.seq, tag_ch, (unsigned)(tm / 60U), (unsigned)(tm % 60U),
                        rec.hr_avg, rec.hr_max, rec.spo2_min,
                        (unsigned)(rec.temp_max_x10 / 10U), (unsigned)(rec.temp_max_x10 % 10U),
                        rec.steps, rec.falls, rec.alarms);
                Comm_SendString(out);
            }
        }
        Comm_SendString("DUMP_END\r\n");
        return;
    }
    if (ci_starts_with(p, "PAGE "))
    {
        int pg = atoi(p + 5);
        if (pg >= 0 && pg < PAGE_COUNT)
        {
            App_SetPage((u8)pg);
            sprintf(out, "OK PAGE=%d\r\n", pg);
            Comm_SendString(out);
        }
        else
        {
            Comm_SendString("ERR PAGE 0..2\r\n");
        }
        return;
    }
    if (ci_starts_with(p, "TIME="))
    {
        const char *ts = p + 5;
        if (strlen(ts) >= 5 && ts[2] == ':')
        {
            int hh = atoi(ts);
            int mm = atoi(ts + 3);
            int ss = (strlen(ts) >= 8 && ts[5] == ':') ? atoi(ts + 6) : 0;
            if (hh >= 0 && hh < 24 && mm >= 0 && mm < 60 && ss >= 0 && ss < 60)
            {
                sys_state.clock_tod_sec = (u32)hh * 3600UL + (u32)mm * 60UL + (u32)ss;
                sys_state.time_synced = 1;
                BKP_State_Save();
                sprintf(out, "OK TIME=%02d:%02d:%02d\r\n", hh, mm, ss);
                Comm_SendString(out);
                return;
            }
        }
        Comm_SendString("ERR TIME FORMAT (USE HH:MM:SS)\r\n");
        return;
    }

    if (ci_starts_with(p, "SET ")) p += 4;
    while (*p == ' ') p++;

    {
        int v;
        u8 changed = 0;
        if (ci_starts_with(p, "HR_MAX=") || ci_starts_with(p, "HRMAX="))
        {
            v = atoi(strchr(p, '=') + 1);
            if (v >= (int)hr_min + 5 && v <= 220) { hr_max = (u8)v; changed = 1; }
        }
        else if (ci_starts_with(p, "HR_MIN=") || ci_starts_with(p, "HRMIN="))
        {
            v = atoi(strchr(p, '=') + 1);
            if (v >= 20 && v <= (int)hr_max - 5) { hr_min = (u8)v; changed = 1; }
        }
        else if (ci_starts_with(p, "SPO2="))
        {
            v = atoi(strchr(p, '=') + 1);
            if (v >= 50 && v <= 100) { spo2_min = (u8)v; changed = 1; }
        }
        else if (ci_starts_with(p, "T_MAX=") || ci_starts_with(p, "TMAX="))
        {
            v = atoi(strchr(p, '=') + 1);
            if (v >= (int)temperature_min + 2 && v <= 60) { temperature_max = (u8)v; changed = 1; }
        }
        else if (ci_starts_with(p, "T_MIN=") || ci_starts_with(p, "TMIN="))
        {
            v = atoi(strchr(p, '=') + 1);
            if (v >= 0 && v <= (int)temperature_max - 2) { temperature_min = (u8)v; changed = 1; }
        }
        else
        {
            Comm_SendString("ERR UNKNOWN CMD (SEND HELP)\r\n");
            return;
        }

        if (changed)
        {
            Config_Save(hr_min, hr_max, spo2_min, temperature_min, temperature_max);
            if (sys_state.page == 1 && !Fall_IsSOSActive()) Alarm_ShowPage();
            sprintf(out, "OK CFG HR=%u-%u SPO2>=%u T=%u-%u\r\n",
                    hr_min, hr_max, spo2_min, temperature_min, temperature_max);
            Comm_SendString(out);
        }
        else
        {
            Comm_SendString("ERR RANGE\r\n");
        }
    }
}

void Comm_Poll(void)
{
    int ch;
    while (1)
    {
        ch = USB_CDC_ReadByte();
        if (ch < 0 && uart_rx_tail != uart_rx_head)
        {
            ch = uart_rx_buf[uart_rx_tail];
            uart_rx_tail = (u16)((uart_rx_tail + 1U) % UART1_RX_BUF_SIZE);
        }
        if (ch < 0) break;

        if (ch == '\r' || ch == '\n')
        {
            if (cmd_len > 0)
            {
                cmd_line[cmd_len] = '\0';
                cmd_len = 0;
                Comm_HandleLine(cmd_line);
            }
        }
        else if (cmd_len < (sizeof(cmd_line) - 1U))
        {
            cmd_line[cmd_len++] = (char)ch;
        }
    }
}
