#include "usb_cdc.h"
#include "delay.h"
#include <string.h>

/* STM32F103 全速 USB 设备外设直接寄存器定义 */
#define USB_REG_BASE      0x40005C00UL
#define USB_PMA_BASE      0x40006000UL

#define USB_EPR(ep)       (*(volatile u16 *)(USB_REG_BASE + ((u32)(ep) * 4UL)))
#define USB_CNTR          (*(volatile u16 *)(USB_REG_BASE + 0x40UL))
#define USB_ISTR          (*(volatile u16 *)(USB_REG_BASE + 0x44UL))
#define USB_FNR           (*(volatile u16 *)(USB_REG_BASE + 0x48UL))
#define USB_DADDR         (*(volatile u16 *)(USB_REG_BASE + 0x4CUL))
#define USB_BTABLE        (*(volatile u16 *)(USB_REG_BASE + 0x50UL))

#define PMA_ADDR_TX(ep)   (*(volatile u16 *)(USB_PMA_BASE + ((u32)(ep) * 16UL) + 0UL))
#define PMA_COUNT_TX(ep)  (*(volatile u16 *)(USB_PMA_BASE + ((u32)(ep) * 16UL) + 4UL))
#define PMA_ADDR_RX(ep)   (*(volatile u16 *)(USB_PMA_BASE + ((u32)(ep) * 16UL) + 8UL))
#define PMA_COUNT_RX(ep)  (*(volatile u16 *)(USB_PMA_BASE + ((u32)(ep) * 16UL) + 12UL))

#define EP_STAT_DIS       0x0U
#define EP_STAT_STALL     0x1U
#define EP_STAT_NAK       0x2U
#define EP_STAT_VALID     0x3U

#define EP_TYPE_BULK      (0x0U << 9)
#define EP_TYPE_CONTROL   (0x1U << 9)
#define EP_TYPE_INT       (0x3U << 9)

/* BTABLE 位于 PMA 0x0000..0x003F，缓冲区紧随其后 */
#define EP0_RX_PMA        0x0040U
#define EP0_TX_PMA        0x0080U
#define EP1_TX_PMA        0x00C0U   /* CDC Bulk IN  0x81 */
#define EP2_TX_PMA        0x0100U   /* CDC Int  IN  0x82 */
#define EP3_RX_PMA        0x0110U   /* CDC Bulk OUT 0x03 */
#define EP_RX_64B_CFG     0x8400U   /* BL_SIZE=1(32B), NUM_BLOCK=1 => 64B */

#define USB_TX_BUF_SIZE   512U
#define USB_RX_BUF_SIZE   128U

volatile u8 usb_cdc_host_ready = 0;

static volatile u8  usb_configured = 0;
static volatile u8  usb_tx_busy = 0;
static volatile u8  usb_need_zlp = 0;
static volatile u8  usb_dtr = 0;
static volatile u8  pending_addr = 0;

static volatile u8  ep0_out_stage = 0;   /* 1 = 等待 SET_LINE_CODING 数据 */
static const u8    *ep0_tx_ptr = 0;
static u16          ep0_tx_left = 0;

static u8  usb_tx_buf[USB_TX_BUF_SIZE];
static volatile u16 tx_head = 0, tx_tail = 0;

static u8  usb_rx_buf[USB_RX_BUF_SIZE];
static volatile u16 rx_head = 0, rx_tail = 0;

static u8 line_coding[7] = {
    0x00, 0xC2, 0x01, 0x00, /* 115200 bps */
    0x00,                   /* 1 stop bit */
    0x00,                   /* No parity */
    0x08                    /* 8 data bits */
};

/* 标准 USB 设备描述符 (ST VCP VID/PID 免驱匹配 Win10/11 usbser.sys) */
static const u8 dev_desc[18] = {
    18, 0x01,               /* bLength, DEVICE */
    0x00, 0x02,             /* USB 2.0 */
    0x02, 0x02, 0x00,       /* CDC Class, ACM */
    64,                     /* bMaxPacketSize0 */
    0x83, 0x04,             /* idVendor  = 0x0483 */
    0x40, 0x57,             /* idProduct = 0x5740 */
    0x00, 0x02,             /* bcdDevice */
    1, 2, 3,                /* iManufacturer, iProduct, iSerialNumber */
    1                       /* bNumConfigurations */
};

static const u8 cfg_desc[67] = {
    /* Configuration Descriptor */
    9, 0x02, 67, 0x00, 2, 1, 0, 0x80, 50,
    /* Interface 0: CDC Control */
    9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    /* Header Functional */
    5, 0x24, 0x00, 0x10, 0x01,
    /* Call Management Functional */
    5, 0x24, 0x01, 0x00, 1,
    /* ACM Functional */
    4, 0x24, 0x02, 0x02,
    /* Union Functional */
    5, 0x24, 0x06, 0, 1,
    /* EP2 Interrupt IN (0x82) */
    7, 0x05, 0x82, 0x03, 8, 0, 0x10,
    /* Interface 1: CDC Data */
    9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    /* EP3 Bulk OUT (0x03) */
    7, 0x05, 0x03, 0x02, 64, 0, 0,
    /* EP1 Bulk IN (0x81) */
    7, 0x05, 0x81, 0x02, 64, 0, 0
};

static const u8 str_lang[4] = { 4, 0x03, 0x09, 0x04 };
static const u8 str_mfg[18] = {
    18, 0x03, 'S',0,'T',0,'M',0,'3',0,'2',0,'B',0,'n',0,'d',0
};
static const u8 str_prod[30] = {
    30, 0x03, 'S',0,'m',0,'a',0,'r',0,'t',0,'W',0,'r',0,'i',0,'s',0,'t',0,'U',0,'S',0,'B',0,'C',0
};
static const u8 str_ser[18] = {
    18, 0x03, '2',0,'0',0,'2',0,'6',0,'0',0,'1',0,'0',0,'1',0
};

static void pma_write(u16 pma_addr, const u8 *src, u16 len)
{
    u16 words = (u16)((len + 1U) >> 1);
    volatile u16 *dst = (volatile u16 *)(USB_PMA_BASE + ((u32)pma_addr * 2UL));
    u16 i;
    for (i = 0; i < words; i++)
    {
        u16 lo = src[i * 2U];
        u16 hi = ((i * 2U + 1U) < len) ? src[i * 2U + 1U] : 0;
        dst[i * 2U] = (u16)(lo | (hi << 8));
    }
}

static void pma_read(u16 pma_addr, u8 *dst, u16 len)
{
    u16 words = (u16)((len + 1U) >> 1);
    volatile const u16 *src = (volatile const u16 *)(USB_PMA_BASE + ((u32)pma_addr * 2UL));
    u16 i;
    for (i = 0; i < words; i++)
    {
        u16 w = src[i * 2U];
        dst[i * 2U] = (u8)(w & 0xFF);
        if ((i * 2U + 1U) < len)
            dst[i * 2U + 1U] = (u8)(w >> 8);
    }
}

static void ep_set_tx_stat(u8 ep, u16 stat)
{
    u16 val = USB_EPR(ep);
    u16 tog = (u16)((val ^ (stat << 4)) & USB_EP0R_STAT_TX);
    USB_EPR(ep) = (u16)((val & 0x070FU) | 0x8080U | tog);
}

static void ep_set_rx_stat(u8 ep, u16 stat)
{
    u16 val = USB_EPR(ep);
    u16 tog = (u16)((val ^ (stat << 12)) & USB_EP0R_STAT_RX);
    USB_EPR(ep) = (u16)((val & 0x070FU) | 0x8080U | tog);
}

static void ep_clear_ctr_rx(u8 ep)
{
    u16 val = USB_EPR(ep);
    USB_EPR(ep) = (u16)((val & 0x070FU) | 0x0080U);
}

static void ep_clear_ctr_tx(u8 ep)
{
    u16 val = USB_EPR(ep);
    USB_EPR(ep) = (u16)((val & 0x070FU) | 0x8000U);
}

static void ep_init_cfg(u8 ep, u16 type, u8 addr, u16 rx_stat, u16 tx_stat)
{
    u16 cur = USB_EPR(ep);
    u16 want = (u16)((rx_stat << 12) | type | (tx_stat << 4) | (addr & 0x0FU));
    /* 把 toggle 位按需翻转并清 DTOG_RX/TX 为 0 */
    u16 tog = (u16)((cur ^ want) & (USB_EP0R_STAT_RX | USB_EP0R_STAT_TX |
                                    USB_EP0R_DTOG_RX | USB_EP0R_DTOG_TX));
    USB_EPR(ep) = (u16)(type | (addr & 0x0FU) | tog);
}

static void ep0_send_next(void)
{
    u16 n = ep0_tx_left;
    if (n > 64U) n = 64U;
    if (n > 0U && ep0_tx_ptr != 0)
    {
        pma_write(EP0_TX_PMA, ep0_tx_ptr, n);
        ep0_tx_ptr += n;
        ep0_tx_left -= n;
    }
    PMA_COUNT_TX(0) = n;
    ep_set_tx_stat(0, EP_STAT_VALID);
}

static void ep0_send_zlp(void)
{
    ep0_tx_ptr = 0;
    ep0_tx_left = 0;
    PMA_COUNT_TX(0) = 0;
    ep_set_tx_stat(0, EP_STAT_VALID);
}

static void usb_kick_tx(void)
{
    u8 chunk[64];
    u16 n = 0;

    if (!usb_configured || usb_tx_busy) return;

    while (tx_tail != tx_head && n < 64U)
    {
        chunk[n++] = usb_tx_buf[tx_tail];
        tx_tail = (u16)((tx_tail + 1U) % USB_TX_BUF_SIZE);
    }

    if (n > 0U)
    {
        pma_write(EP1_TX_PMA, chunk, n);
        PMA_COUNT_TX(1) = n;
        usb_tx_busy = 1;
        usb_need_zlp = (n == 64U && tx_tail == tx_head) ? 1 : 0;
        ep_set_tx_stat(1, EP_STAT_VALID);
    }
    else if (usb_need_zlp)
    {
        usb_need_zlp = 0;
        PMA_COUNT_TX(1) = 0;
        usb_tx_busy = 1;
        ep_set_tx_stat(1, EP_STAT_VALID);
    }
}

static void usb_reset_endpoints(void)
{
    USB_BTABLE = 0x0000U;

    PMA_ADDR_TX(0)  = EP0_TX_PMA;
    PMA_COUNT_TX(0) = 0;
    PMA_ADDR_RX(0)  = EP0_RX_PMA;
    PMA_COUNT_RX(0) = EP_RX_64B_CFG;

    PMA_ADDR_TX(1)  = EP1_TX_PMA;
    PMA_COUNT_TX(1) = 0;
    PMA_ADDR_RX(1)  = 0;
    PMA_COUNT_RX(1) = 0;

    PMA_ADDR_TX(2)  = EP2_TX_PMA;
    PMA_COUNT_TX(2) = 0;
    PMA_ADDR_RX(2)  = 0;
    PMA_COUNT_RX(2) = 0;

    PMA_ADDR_TX(3)  = 0;
    PMA_COUNT_TX(3) = 0;
    PMA_ADDR_RX(3)  = EP3_RX_PMA;
    PMA_COUNT_RX(3) = EP_RX_64B_CFG;

    ep_init_cfg(0, EP_TYPE_CONTROL, 0, EP_STAT_VALID, EP_STAT_NAK);
    ep_init_cfg(1, EP_TYPE_BULK,    1, EP_STAT_DIS,   EP_STAT_NAK);
    ep_init_cfg(2, EP_TYPE_INT,     2, EP_STAT_DIS,   EP_STAT_NAK);
    ep_init_cfg(3, EP_TYPE_BULK,    3, EP_STAT_VALID, EP_STAT_DIS);

    pending_addr = 0;
    usb_configured = 0;
    usb_tx_busy = 0;
    usb_need_zlp = 0;
    usb_dtr = 0;
    usb_cdc_host_ready = 0;
    ep0_out_stage = 0;
    ep0_tx_ptr = 0;
    ep0_tx_left = 0;
    tx_head = tx_tail = 0;

    USB_DADDR = 0x0080U;   /* 使能功能, 地址=0 */
}

static void usb_handle_setup(void)
{
    u8 setup[8];
    u8  bmReq, bReq;
    u16 wVal, wLen;
    const u8 *resp = 0;
    u16 resp_len = 0;
    static u8 status_zero[2] = { 0, 0 };

    pma_read(EP0_RX_PMA, setup, 8);
    bmReq = setup[0];
    bReq  = setup[1];
    wVal  = (u16)(setup[2] | ((u16)setup[3] << 8));
    wLen  = (u16)(setup[6] | ((u16)setup[7] << 8));

    if ((bmReq & 0x60U) == 0x00U)   /* 标准请求 */
    {
        switch (bReq)
        {
            case 0x06U: /* GET_DESCRIPTOR */
            {
                u8 desc_type = (u8)(wVal >> 8);
                u8 desc_idx  = (u8)(wVal & 0xFF);
                if (desc_type == 1U)      { resp = dev_desc; resp_len = sizeof(dev_desc); }
                else if (desc_type == 2U) { resp = cfg_desc; resp_len = sizeof(cfg_desc); }
                else if (desc_type == 3U)
                {
                    if (desc_idx == 0U)      { resp = str_lang; resp_len = sizeof(str_lang); }
                    else if (desc_idx == 1U) { resp = str_mfg;  resp_len = sizeof(str_mfg);  }
                    else if (desc_idx == 2U) { resp = str_prod; resp_len = sizeof(str_prod); }
                    else if (desc_idx == 3U) { resp = str_ser;  resp_len = sizeof(str_ser);  }
                }
                break;
            }
            case 0x05U: /* SET_ADDRESS */
                pending_addr = (u8)(wVal & 0x7FU);
                ep0_send_zlp();
                ep_set_rx_stat(0, EP_STAT_VALID);
                return;

            case 0x09U: /* SET_CONFIGURATION */
                usb_configured = (wVal != 0U) ? 1 : 0;
                if (usb_configured)
                {
                    ep_init_cfg(1, EP_TYPE_BULK, 1, EP_STAT_DIS,   EP_STAT_NAK);
                    ep_init_cfg(2, EP_TYPE_INT,  2, EP_STAT_DIS,   EP_STAT_NAK);
                    ep_init_cfg(3, EP_TYPE_BULK, 3, EP_STAT_VALID, EP_STAT_DIS);
                    usb_tx_busy = 0;
                    usb_need_zlp = 0;
                }
                ep0_send_zlp();
                ep_set_rx_stat(0, EP_STAT_VALID);
                return;

            case 0x08U: /* GET_CONFIGURATION */
                status_zero[0] = usb_configured;
                resp = status_zero;
                resp_len = 1;
                break;

            case 0x00U: /* GET_STATUS */
                status_zero[0] = 0;
                status_zero[1] = 0;
                resp = status_zero;
                resp_len = 2;
                break;

            case 0x01U: /* CLEAR_FEATURE */
            case 0x0BU: /* SET_INTERFACE */
                ep0_send_zlp();
                ep_set_rx_stat(0, EP_STAT_VALID);
                return;

            default:
                break;
        }
    }
    else if ((bmReq & 0x60U) == 0x20U) /* CDC 类请求 */
    {
        if (bReq == 0x20U) /* SET_LINE_CODING (主机随后发 7B OUT) */
        {
            ep0_out_stage = 1;
            PMA_COUNT_RX(0) = EP_RX_64B_CFG;
            ep_set_rx_stat(0, EP_STAT_VALID);
            return;
        }
        else if (bReq == 0x21U) /* GET_LINE_CODING */
        {
            resp = line_coding;
            resp_len = sizeof(line_coding);
        }
        else if (bReq == 0x22U) /* SET_CONTROL_LINE_STATE */
        {
            usb_dtr = (wVal & 0x01U) ? 1 : 0;
            usb_cdc_host_ready = (usb_configured && usb_dtr) ? 1 : usb_cdc_host_ready;
            ep0_send_zlp();
            ep_set_rx_stat(0, EP_STAT_VALID);
            return;
        }
    }

    if (resp != 0 && resp_len > 0U)
    {
        if (resp_len > wLen) resp_len = wLen;
        ep0_tx_ptr = resp;
        ep0_tx_left = resp_len;
        ep0_send_next();
        ep_set_rx_stat(0, EP_STAT_VALID);
    }
    else
    {
        ep_set_tx_stat(0, EP_STAT_STALL);
        ep_set_rx_stat(0, EP_STAT_VALID);
    }
}

void USB_CDC_Init(u8 pulse_disconnect)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_1Div5);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USB, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    if (pulse_disconnect)
    {
        /* 拉低 PA12 (D+) ~20ms，促使电脑立即重新枚举 COM 口 */
        GPIO_InitStructure.GPIO_Pin = GPIO_Pin_12;
        GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
        GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
        GPIO_Init(GPIOA, &GPIO_InitStructure);
        GPIO_ResetBits(GPIOA, GPIO_Pin_12);
        delay_ms(20);
    }

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_11 | GPIO_Pin_12;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    USB_CNTR = USB_CNTR_FRES;   /* 先保持强制复位与通电 */
    delay_us(10);
    USB_CNTR = 0x0000U;
    USB_ISTR = 0x0000U;

    usb_reset_endpoints();

    NVIC_InitStructure.NVIC_IRQChannel = USB_LP_CAN1_RX0_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    USB_CNTR = USB_CNTR_CTRM | USB_CNTR_RESETM;
}

u8 USB_CDC_IsConfigured(void)
{
    return usb_configured;
}

u16 USB_CDC_Send(const u8 *buf, u16 len)
{
    u16 i, sent = 0;

    if (!usb_configured || buf == 0 || len == 0) return 0;

    NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);
    for (i = 0; i < len; i++)
    {
        u16 next = (u16)((tx_head + 1U) % USB_TX_BUF_SIZE);
        if (next == tx_tail) break;   /* 缓冲满时不堵塞传感器主循环 */
        usb_tx_buf[tx_head] = buf[i];
        tx_head = next;
        sent++;
    }
    if (!usb_tx_busy)
    {
        usb_kick_tx();
    }
    NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
    return sent;
}

u16 USB_CDC_SendString(const char *str)
{
    if (str == 0) return 0;
    return USB_CDC_Send((const u8 *)str, (u16)strlen(str));
}

u16 USB_CDC_Available(void)
{
    return (u16)((rx_head - rx_tail + USB_RX_BUF_SIZE) % USB_RX_BUF_SIZE);
}

int USB_CDC_ReadByte(void)
{
    u8 c;
    if (rx_tail == rx_head) return -1;
    c = usb_rx_buf[rx_tail];
    rx_tail = (u16)((rx_tail + 1U) % USB_RX_BUF_SIZE);
    return (int)c;
}

void USB_CDC_IRQHandler(void)
{
    u16 istr = USB_ISTR;

    if (istr & USB_ISTR_RESET)
    {
        USB_ISTR = (u16)(~USB_ISTR_RESET);
        usb_reset_endpoints();
    }

    while ((istr = USB_ISTR) & USB_ISTR_CTR)
    {
        u8 ep = (u8)(istr & USB_ISTR_EP_ID);

        if (ep == 0U)
        {
            u16 epr = USB_EPR(0);
            if (epr & USB_EP0R_CTR_TX)
            {
                ep_clear_ctr_tx(0);
                if (pending_addr != 0U)
                {
                    USB_DADDR = (u16)(0x0080U | pending_addr);
                    pending_addr = 0;
                }
                else if (ep0_tx_left > 0U && !(epr & USB_EP0R_SETUP))
                {
                    ep0_send_next();
                }
            }
            if (USB_EPR(0) & USB_EP0R_CTR_RX)
            {
                u16 epr_rx = USB_EPR(0);
                ep_clear_ctr_rx(0);
                if (epr_rx & USB_EP0R_SETUP)
                {
                    usb_handle_setup();
                }
                else
                {
                    u16 rx_cnt = (u16)(PMA_COUNT_RX(0) & 0x03FFU);
                    if (ep0_out_stage)
                    {
                        if (rx_cnt >= 7U) pma_read(EP0_RX_PMA, line_coding, 7);
                        ep0_out_stage = 0;
                        ep0_send_zlp();
                    }
                    PMA_COUNT_RX(0) = EP_RX_64B_CFG;
                    ep_set_rx_stat(0, EP_STAT_VALID);
                }
            }
        }
        else if (ep == 1U)
        {
            if (USB_EPR(1) & USB_EP0R_CTR_TX)
            {
                ep_clear_ctr_tx(1);
                usb_tx_busy = 0;
                usb_cdc_host_ready = 1;
                usb_kick_tx();
            }
        }
        else if (ep == 3U)
        {
            if (USB_EPR(3) & USB_EP0R_CTR_RX)
            {
                u8  tmp[64];
                u16 cnt = (u16)(PMA_COUNT_RX(3) & 0x03FFU);
                u16 i;

                ep_clear_ctr_rx(3);
                if (cnt > 64U) cnt = 64U;
                if (cnt > 0U)
                {
                    pma_read(EP3_RX_PMA, tmp, cnt);
                    for (i = 0; i < cnt; i++)
                    {
                        u16 next = (u16)((rx_head + 1U) % USB_RX_BUF_SIZE);
                        if (next != rx_tail)
                        {
                            usb_rx_buf[rx_head] = tmp[i];
                            rx_head = next;
                        }
                    }
                    usb_cdc_host_ready = 1;
                }
                PMA_COUNT_RX(3) = EP_RX_64B_CFG;
                ep_set_rx_stat(3, EP_STAT_VALID);
            }
        }
        else
        {
            if (USB_EPR(ep) & USB_EP0R_CTR_RX) ep_clear_ctr_rx(ep);
            if (USB_EPR(ep) & USB_EP0R_CTR_TX) ep_clear_ctr_tx(ep);
        }
    }
}
