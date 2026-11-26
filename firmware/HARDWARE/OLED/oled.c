#include "oled.h"
#include "stdlib.h"
#include "string.h"
#include "oledfont.h"
#include "delay.h"
#include "stdio.h"
#include "alarm.h"       // A1 报警页
#include "flash_hist.h"  // A6 历史页
#include "bkp_state.h"   // 运行计时/对时/USB状态

u8 OLED_GRAM[144][8];
static u8 oled_batch_depth = 0;

extern u8 hr,spo2;
extern short temperature;
extern u16 step_num;
extern u8 ds18b20_ok;
extern u8 usb_cdc_host_ready;

//反显函数
void OLED_ColorTurn(u8 i)
{
	if(i==0)
		{
			OLED_WR_Byte(0xA6,OLED_CMD);//正常显示
		}
	if(i==1)
		{
			OLED_WR_Byte(0xA7,OLED_CMD);//反色显示
		}
}

//屏幕旋转180度
void OLED_DisplayTurn(u8 i)
{
	if(i==0)
		{
			OLED_WR_Byte(0xC8,OLED_CMD);//正常显示
			OLED_WR_Byte(0xA1,OLED_CMD);
		}
	if(i==1)
		{
			OLED_WR_Byte(0xC0,OLED_CMD);//反转显示
			OLED_WR_Byte(0xA0,OLED_CMD);
		}
}

//延时（volatile 防止高优化等级将空循环优化掉导致 OLED I2C 时序过快花屏）
void IIC_delay(void)
{
	volatile u8 t=6;
	while(t--);
}

//起始信号
void OLED_I2C_Start(void)
{
	OLED_SDA_Set();
	OLED_SCL_Set();
	IIC_delay();
	OLED_SDA_Clr();
	IIC_delay();
	OLED_SCL_Clr();
	IIC_delay();
}

//结束信号
void OLED_I2C_Stop(void)
{
	OLED_SDA_Clr();
	OLED_SCL_Set();
	IIC_delay();
	OLED_SDA_Set();
	IIC_delay();
}

//等待信号响应
void OLED_I2C_WaitAck(void) //测数据信号的电平
{
	OLED_SDA_Set();
	IIC_delay();
	OLED_SCL_Set();
	IIC_delay();
	OLED_SCL_Clr();
	IIC_delay();
}

//写入一个字节
void OLED_Send_Byte(u8 dat)
{
	u8 i;
	for(i=0;i<8;i++)
	{
		if(dat&0x80)//将dat的8位从最高位依次写入
		{
			OLED_SDA_Set();
    }
		else
		{
			OLED_SDA_Clr();
    }
		IIC_delay();
		OLED_SCL_Set();
		IIC_delay();
		OLED_SCL_Clr();//将时钟信号设置为低电平
		dat<<=1;
  }
}

//发送一个字节
//mode:数据/命令标志 0,表示命令;1,表示数据;
void OLED_WR_Byte(u8 dat,u8 mode)
{
	OLED_I2C_Start();
	OLED_Send_Byte(0x78);
	OLED_I2C_WaitAck();
	if(mode){OLED_Send_Byte(0x40);}
  else{OLED_Send_Byte(0x00);}
	OLED_I2C_WaitAck();
	OLED_Send_Byte(dat);
	OLED_I2C_WaitAck();
	OLED_I2C_Stop();
}

//开启OLED显示 
void OLED_DisPlay_On(void)
{
	OLED_WR_Byte(0x8D,OLED_CMD);//电荷泵使能
	OLED_WR_Byte(0x14,OLED_CMD);//开启电荷泵
	OLED_WR_Byte(0xAF,OLED_CMD);//点亮屏幕
}

//关闭OLED显示 
void OLED_DisPlay_Off(void)
{
	OLED_WR_Byte(0x8D,OLED_CMD);//电荷泵使能
	OLED_WR_Byte(0x10,OLED_CMD);//关闭电荷泵
	OLED_WR_Byte(0xAE,OLED_CMD);//关闭屏幕
}

//更新显存到OLED	
void OLED_Refresh(void)
{
	u8 i,n;
	for(i=0;i<8;i++)
	{
		OLED_WR_Byte(0xb0+i,OLED_CMD); //设置行起始地址
		OLED_WR_Byte(0x00,OLED_CMD);   //设置低列起始地址
		OLED_WR_Byte(0x10,OLED_CMD);   //设置高列起始地址
		OLED_I2C_Start();
		OLED_Send_Byte(0x78);
		OLED_I2C_WaitAck();
		OLED_Send_Byte(0x40);
		OLED_I2C_WaitAck();
		for(n=0;n<128;n++)
		{
			OLED_Send_Byte(OLED_GRAM[n][i]);
			OLED_I2C_WaitAck();
		}
		OLED_I2C_Stop();
  }
}
static void OLED_RefreshIfAuto(void)
{
	if (oled_batch_depth == 0)
	{
		OLED_Refresh();
	}
}

void OLED_BeginUpdate(void)
{
	oled_batch_depth++;
}

void OLED_EndUpdate(void)
{
	if (oled_batch_depth > 0)
	{
		oled_batch_depth--;
		if (oled_batch_depth == 0)
		{
			OLED_Refresh();
		}
	}
}

//清屏函数
void OLED_Clear(void)
{
	u8 i,n;
	for(i=0;i<8;i++)
	{
	   for(n=0;n<128;n++)
			{
			 OLED_GRAM[n][i]=0;//清除所有数据
			}
  }
	OLED_RefreshIfAuto();//批量更新模式下跳过立即刷屏，避免闪烁和多次 I2C
}

//画点 
//x:0~127
//y:0~63
//t:1 填充 0,清空	
void OLED_DrawPoint(u8 x,u8 y,u8 t)
{
	u8 i,m,n;
	if (x >= 144 || y >= 64) return;
	i=y/8;
	m=y%8;
	n=1<<m;
	if(t){OLED_GRAM[x][i]|=n;}
	else
	{
		OLED_GRAM[x][i]=~OLED_GRAM[x][i];
		OLED_GRAM[x][i]|=n;
		OLED_GRAM[x][i]=~OLED_GRAM[x][i];
	}
//	OLED_Refresh();
}

//画线
//x1,y1:起点坐标
//x2,y2:结束坐标
void OLED_DrawLine(u8 x1,u8 y1,u8 x2,u8 y2,u8 mode)
{
	u16 t; 
	int xerr=0,yerr=0,delta_x,delta_y,distance;
	int incx,incy,uRow,uCol;
	delta_x=x2-x1; //计算坐标增量 
	delta_y=y2-y1;
	uRow=x1;//画线起点坐标
	uCol=y1;
	if(delta_x>0)incx=1; //设置单步方向 
	else if (delta_x==0)incx=0;//垂直线 
	else {incx=-1;delta_x=-delta_x;}
	if(delta_y>0)incy=1;
	else if (delta_y==0)incy=0;//水平线 
	else {incy=-1;delta_y=-delta_x;}
	if(delta_x>delta_y)distance=delta_x; //选取基本增量坐标轴 
	else distance=delta_y;
	for(t=0;t<distance+1;t++)
	{
		OLED_DrawPoint(uRow,uCol,mode);//画点
		xerr+=delta_x;
		yerr+=delta_y;
		if(xerr>distance)
		{
			xerr-=distance;
			uRow+=incx;
		}
		if(yerr>distance)
		{
			yerr-=distance;
			uCol+=incy;
		}
	}
//	OLED_Refresh();
}
//x,y:圆心坐标
//r:圆的半径
void OLED_DrawCircle(u8 x,u8 y,u8 r)
{
	int a, b,num;
    a = 0;
    b = r;
    while(2 * b * b >= r * r)      
    {
        OLED_DrawPoint(x + a, y - b,1);
        OLED_DrawPoint(x - a, y - b,1);
        OLED_DrawPoint(x - a, y + b,1);
        OLED_DrawPoint(x + a, y + b,1);
 
        OLED_DrawPoint(x + b, y + a,1);
        OLED_DrawPoint(x + b, y - a,1);
        OLED_DrawPoint(x - b, y - a,1);
        OLED_DrawPoint(x - b, y + a,1);
        
        a++;
        num = (a * a + b * b) - r*r;//计算画的点离圆心的距离
        if(num > 0)
        {
            b--;
            a--;
        }
    }
//		OLED_Refresh();
}



//在指定位置显示一个字符,包括部分字符
//x:0~127
//y:0~63
//size1:选择字体 6x8/6x12/8x16/12x24
//mode:0,反色显示;1,正常显示
static void OLED_DrawCharBuf(u8 x,u8 y,u8 chr,u8 size1,u8 mode)
{
	u8 i,m,temp,size2,chr1;
	u8 x0=x,y0=y;
	if(size1==8)size2=6;
	else size2=(size1/8+((size1%8)?1:0))*(size1/2);  //得到字体一个字符对应点阵集所占的字节数
	chr1=chr-' ';  //计算偏移后的值
	for(i=0;i<size2;i++)
	{
		if(size1==16)
		{temp=asc2_1608[chr1][i];} //调用1608字体
#if OLED_ENABLE_EXTRA_FONTS
		else if(size1==8)
		{temp=asc2_0806[chr1][i];} //调用0806字体
		else if(size1==12)
		{temp=asc2_1206[chr1][i];} //调用1206字体
		else if(size1==24)
		{temp=asc2_2412[chr1][i];} //调用2412字体
#endif
		else return;
		for(m=0;m<8;m++)
		{
			if(temp&0x01)OLED_DrawPoint(x,y,mode);
			else OLED_DrawPoint(x,y,!mode);
			temp>>=1;
			y++;
		}
		x++;
		if((size1!=8)&&((x-x0)==size1/2))
		{x=x0;y0=y0+8;}
		y=y0;
  }
}

void OLED_ShowChar(u8 x,u8 y,u8 chr,u8 size1,u8 mode)
{
	OLED_DrawCharBuf(x,y,chr,size1,mode);
	OLED_RefreshIfAuto();
}


//显示字符串
//x,y:起点坐标  
//size1:字体大小 
//*chr:字符串起始地址 
//mode:0,反色显示;1,正常显示
void OLED_ShowString(u8 x,u8 y,u8 *chr,u8 size1,u8 mode)
{
	while((*chr>=' ')&&(*chr<='~'))//判断是不是非法字符!
	{
		OLED_DrawCharBuf(x,y,*chr,size1,mode);
		if(size1==8)x+=6;
		else x+=size1/2;
		chr++;
  }
	OLED_RefreshIfAuto();
}

//m^n
u32 OLED_Pow(u8 m,u8 n)
{
	u32 result=1;
	while(n--)
	{
	  result*=m;
	}
	return result;
}

//显示数字
//x,y :起点坐标
//num :要显示的数字
//len :数字的位数
//size:字体大小
//mode:0,反色显示;1,正常显示
void OLED_ShowNum(u8 x,u8 y,u32 num,u8 len,u8 size1,u8 mode)
{
	u8 t,temp,m=0;
	if(size1==8)m=2;
	for(t=0;t<len;t++)
	{
		temp=(num/OLED_Pow(10,len-t-1))%10;
			if(temp==0)
			{
				OLED_DrawCharBuf(x+(size1/2+m)*t,y,'0',size1,mode);
      }
			else 
			{
			  OLED_DrawCharBuf(x+(size1/2+m)*t,y,temp+'0',size1,mode);
			}
  }
	OLED_RefreshIfAuto();
}

//显示汉字
//x,y:起点坐标
//num:汉字对应的序号
//mode:0,反色显示;1,正常显示
void OLED_ShowChinese(u8 x,u8 y,u8 num,u8 size1,u8 mode)
{
#if OLED_ENABLE_EXTRA_FONTS
	u8 m,temp;
	u8 x0=x,y0=y;
	u16 i,size3=(size1/8+((size1%8)?1:0))*size1;
	for(i=0;i<size3;i++)
	{
		if(size1==16)      {temp=Hzk1[num][i];}
		else if(size1==24) {temp=Hzk2[num][i];}
		else if(size1==32) {temp=Hzk3[num][i];}
		else if(size1==64) {temp=Hzk4[num][i];}
		else return;
		for(m=0;m<8;m++)
		{
			if(temp&0x01)OLED_DrawPoint(x,y,mode);
			else OLED_DrawPoint(x,y,!mode);
			temp>>=1;
			y++;
		}
		x++;
		if((x-x0)==size1)
		{x=x0;y0=y0+8;}
		y=y0;
	}
	OLED_RefreshIfAuto();
#else
	(void)x; (void)y; (void)num; (void)size1; (void)mode;
#endif
}

static void OLED_DrawCnGlyphBuf(u8 x, u8 y, const u8 *glyph, u8 mode)
{
	u8 x0 = x, y0 = y;
	u8 i, m, temp;

	for (i = 0; i < 32; i++)
	{
		temp = glyph[i];
		for (m = 0; m < 8; m++)
		{
			if (temp & 0x01) OLED_DrawPoint(x, y, mode);
			else             OLED_DrawPoint(x, y, !mode);
			temp >>= 1;
			y++;
		}
		x++;
		if ((x - x0) == 16)
		{
			x = x0;
			y0 += 8;
		}
		y = y0;
	}
}

static u8 OLED_DrawMixedBuf(u8 x, u8 y, const char *s, u8 mode)
{
	u16 total = (u16)(sizeof(cn_font_table) / sizeof(cn_font_table[0]));
	u16 idx;

	while (*s && x < 128)
	{
		u8 c = (u8)s[0];
		if (c < 0x80)
		{
			if (x + 8 > 128) break;
			if (c >= ' ' && c <= '~') OLED_DrawCharBuf(x, y, c, 16, mode);
			else                      OLED_DrawCharBuf(x, y, ' ', 16, mode);
			x += 8;
			s += 1;
		}
		else if ((c & 0xE0) == 0xE0 && s[1] != '\0' && s[2] != '\0')
		{
			const u8 *found = 0;
			if (x + 16 > 128) break;
			for (idx = 0; idx < total; idx++)
			{
				if (cn_font_table[idx].utf8[0] == (u8)s[0] &&
				    cn_font_table[idx].utf8[1] == (u8)s[1] &&
				    cn_font_table[idx].utf8[2] == (u8)s[2])
				{
					found = cn_font_table[idx].data;
					break;
				}
			}
			if (found)
			{
				OLED_DrawCnGlyphBuf(x, y, found, mode);
				x += 16;
			}
			else
			{
				OLED_DrawCharBuf(x, y, '?', 16, mode);
				x += 8;
			}
			s += 3;
		}
		else
		{
			s += 1;
		}
	}
	return x;
}

void OLED_ShowCNText(u8 x, u8 y, const char *s, u8 mode)
{
	OLED_DrawMixedBuf(x, y, s, mode);
	OLED_RefreshIfAuto();
}

void OLED_ShowCNLine(u8 y, const char *s)
{
	u8 end_x = OLED_DrawMixedBuf(0, y, s, 1);
	u8 px, py;
	for (px = end_x; px < 128; px++)
	{
		for (py = y; py < (u8)(y + 16) && py < 64; py++)
		{
			OLED_DrawPoint(px, py, 0);
		}
	}
	OLED_RefreshIfAuto();
}

//num 显示汉字的个数
//space 每一遍显示的间隔
//mode:0,反色显示;1,正常显示
void OLED_ScrollDisplay(u8 num,u8 space,u8 mode)
{
	u8 i,n,t=0,m=0,r;
	while(1)
	{
		if(m==0)
		{
	    OLED_ShowChinese(128,24,t,16,mode); //写入一个汉字保存在OLED_GRAM[][]数组中
			t++;
		}
		if(t==num)
			{
				for(r=0;r<16*space;r++)      //显示间隔
				 {
					for(i=1;i<144;i++)
						{
							for(n=0;n<8;n++)
							{
								OLED_GRAM[i-1][n]=OLED_GRAM[i][n];
							}
						}
           OLED_Refresh();
				 }
        t=0;
      }
		m++;
		if(m==16){m=0;}
		for(i=1;i<144;i++)   //实现左移
		{
			for(n=0;n<8;n++)
			{
				OLED_GRAM[i-1][n]=OLED_GRAM[i][n];
			}
		}
		OLED_Refresh();
	}
}

//x,y：起点坐标
//sizex,sizey,图片长宽
//BMP[]：要写入的图片数组
//mode:0,反色显示;1,正常显示
void OLED_ShowPicture(u8 x,u8 y,u8 sizex,u8 sizey,u8 BMP[],u8 mode)
{
	u16 j=0;
	u8 i,n,temp,m;
	u8 x0=x,y0=y;
	sizey=sizey/8+((sizey%8)?1:0);
	for(n=0;n<sizey;n++)
	{
		 for(i=0;i<sizex;i++)
		 {
				temp=BMP[j];
				j++;
				for(m=0;m<8;m++)
				{
					if(temp&0x01)OLED_DrawPoint(x,y,mode);
					else OLED_DrawPoint(x,y,!mode);
					temp>>=1;
					y++;
				}
				x++;
				if((x-x0)==sizex)
				{
					x=x0;
					y0=y0+8;
				}
				y=y0;
     }
	 }
	OLED_Refresh();
}
//OLED的初始化
void OLED_Init(void)
{
	GPIO_InitTypeDef  GPIO_InitStructure;
 	RCC_APB2PeriphClockCmd(OLED_SCL_GPIO_CLK|OLED_SDA_GPIO_CLK, ENABLE);	 //使能A端口时钟
	GPIO_InitStructure.GPIO_Pin = OLED_SCL_PIN;	 
 	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD; 		 //推挽输出
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;//速度50MHz
 	GPIO_Init(OLED_SCL_PROT, &GPIO_InitStructure);	  //初始化PA0,1
 	GPIO_SetBits(OLED_SCL_PROT,OLED_SCL_PIN);
	
	GPIO_InitStructure.GPIO_Pin = OLED_SDA_PIN;	
	GPIO_Init(OLED_SDA_PROT, &GPIO_InitStructure);	  //初始化PA0,1
 	GPIO_SetBits(OLED_SDA_PROT,OLED_SDA_PIN);

	delay_ms(60);
	
	OLED_WR_Byte(0xAE,OLED_CMD);//--turn off oled panel 关闭显示
	OLED_WR_Byte(0x00,OLED_CMD);//---set low column address
	OLED_WR_Byte(0x10,OLED_CMD);//---set high column address
	OLED_WR_Byte(0x40,OLED_CMD);//--set start line address  Set Mapping RAM Display Start Line (0x00~0x3F)
	OLED_WR_Byte(0x81,OLED_CMD);//--set contrast control register
	OLED_WR_Byte(0xCF,OLED_CMD);// Set SEG Output Current Brightness
	OLED_WR_Byte(0xA1,OLED_CMD);//--Set SEG/Column Mapping     0xa0左右反置 0xa1正常
	OLED_WR_Byte(0xC8,OLED_CMD);//Set COM/Row Scan Direction   0xc0上下反置 0xc8正常
	OLED_WR_Byte(0xA6,OLED_CMD);//--set normal display
	OLED_WR_Byte(0xA8,OLED_CMD);//--set multiplex ratio(1 to 64) 设置驱动路数
	OLED_WR_Byte(0x3f,OLED_CMD);//--1/64 duty
	OLED_WR_Byte(0xD3,OLED_CMD);//-set display offset	Shift Mapping RAM Counter (0x00~0x3F)
	OLED_WR_Byte(0x00,OLED_CMD);//-not offset
	OLED_WR_Byte(0xd5,OLED_CMD);//--set display clock divide ratio/oscillator frequency
	OLED_WR_Byte(0x80,OLED_CMD);//--set divide ratio, Set Clock as 100 Frames/Sec
	OLED_WR_Byte(0xD9,OLED_CMD);//--set pre-charge period
	OLED_WR_Byte(0xF1,OLED_CMD);//Set Pre-Charge as 15 Clocks & Discharge as 1 Clock
	OLED_WR_Byte(0xDA,OLED_CMD);//--set com pins hardware configuration
	OLED_WR_Byte(0x12,OLED_CMD);
	OLED_WR_Byte(0xDB,OLED_CMD);//--set vcomh
	OLED_WR_Byte(0x30,OLED_CMD);//Set VCOM Deselect Level
	OLED_WR_Byte(0x20,OLED_CMD);//-Set Page Addressing Mode (0x00/0x01/0x02)
	OLED_WR_Byte(0x02,OLED_CMD);//
	OLED_WR_Byte(0x8D,OLED_CMD);//--set Charge Pump enable/disable
	OLED_WR_Byte(0x14,OLED_CMD);//--set(0x10) disable
	OLED_Clear();
	OLED_WR_Byte(0xAF,OLED_CMD);
}

void OLED_DrawHomeLive(void)
{
	char buf[40];
	u8 hh, mm;
	const char *top_tag;

	OLED_BeginUpdate();

	top_tag = Alarm_IsActive() ? (sys_state.alarm_ack ? "已确认" : "报警中") : "  主页";
	sprintf(buf, "心率:%-3u  %s", hr, top_tag);
	OLED_ShowCNLine(0, buf);

	if (usb_cdc_host_ready)
		sprintf(buf, "血氧:%-3u%% 已连PC", spo2);
	else
		sprintf(buf, "血氧:%u%%", spo2);
	OLED_ShowCNLine(16, buf);

	if (sys_state.time_synced)
	{
		hh = (u8)((sys_state.clock_tod_sec / 3600UL) % 24UL);
		mm = (u8)((sys_state.clock_tod_sec / 60UL) % 60UL);
		sprintf(buf, "步数:%-5u %02u:%02u", step_num, hh, mm);
	}
	else
	{
		sprintf(buf, "步数:%u", step_num);
	}
	OLED_ShowCNLine(32, buf);

	if (!ds18b20_ok)
	{
		OLED_ShowCNLine(48, "体温:传感器异常");
	}
	else if (temperature < 0)
	{
		short v = -temperature;
		sprintf(buf, "体温:-%d.%dC", v / 10, v % 10);
		OLED_ShowCNLine(48, buf);
	}
	else
	{
		sprintf(buf, "体温:%d.%dC", temperature / 10, temperature % 10);
		OLED_ShowCNLine(48, buf);
	}

	OLED_EndUpdate();
}

void OLED_ShowPage(u8 page)
{
	switch(page)
	{
		case 0:
			OLED_DrawHomeLive();
			break;
		case 1:
			Alarm_ShowPage();   // A1 报警与阈值页
			break;
		case 2:
			Hist_ShowPage();    // A6 Flash 历史页
			break;
		default:
			OLED_DrawHomeLive();
			break;
	}
}

