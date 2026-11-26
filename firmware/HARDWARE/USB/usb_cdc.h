#ifndef __USB_CDC_H
#define __USB_CDC_H

#include "stm32f10x.h"

extern volatile u8 usb_cdc_host_ready;   // PC 已配置或打开串口时置 1

void USB_CDC_Init(u8 pulse_disconnect);
u8   USB_CDC_IsConfigured(void);
u16  USB_CDC_Send(const u8 *buf, u16 len);
u16  USB_CDC_SendString(const char *str);
u16  USB_CDC_Available(void);
int  USB_CDC_ReadByte(void);
void USB_CDC_IRQHandler(void);

#endif
