#include "stm32f10x.h"                  // Device header

#include "fall.h"   // A2: TIM3 每 50ms 跌倒检测（原 gizTimerMs 已随 Gizwits 移除）
extern volatile u8 fall_tick_50ms_flag;

void Timer2_Init(u16 interval_ms)
{
	/*开启时钟*/
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);			//开启TIM2的时钟
	
	/*配置时钟源*/
	TIM_InternalClockConfig(TIM2);		//选择TIM2为内部时钟，若不调用此函数，TIM默认也为内部时钟
	
	/*时基单元初始化*/
	TIM_TimeBaseInitTypeDef TIM_TimeBaseInitStructure;				//定义结构体变量
	TIM_TimeBaseInitStructure.TIM_ClockDivision = TIM_CKD_DIV1;		//时钟分频，选择不分频，此参数用于配置滤波器时钟，不影响时基单元功能
	TIM_TimeBaseInitStructure.TIM_CounterMode = TIM_CounterMode_Up;	//计数器模式，选择向上计数
	TIM_TimeBaseInitStructure.TIM_Period = interval_ms*10 - 1;				//计数周期，即ARR的值
	TIM_TimeBaseInitStructure.TIM_Prescaler = 7200 - 1;				//预分频器，即PSC的值
	TIM_TimeBaseInitStructure.TIM_RepetitionCounter = 0;			//重复计数器，高级定时器才会用到
	TIM_TimeBaseInit(TIM2, &TIM_TimeBaseInitStructure);				//将结构体变量交给TIM_TimeBaseInit，配置TIM2的时基单元	
	
	/*中断输出配置*/
	TIM_ClearFlag(TIM2, TIM_FLAG_Update);						//清除定时器更新标志位
																//TIM_TimeBaseInit函数末尾，手动产生了更新事件
																//若不清除此标志位，则开启中断后，会立刻进入一次中断
																//如果不介意此问题，则不清除此标志位也可
	
	TIM_ITConfig(TIM2, TIM_IT_Update, ENABLE);					//开启TIM2的更新中断
	
	/*NVIC配置*/
	NVIC_InitTypeDef NVIC_InitStructure;						//定义结构体变量
	NVIC_InitStructure.NVIC_IRQChannel = TIM2_IRQn;				//选择配置NVIC的TIM2线
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;				//指定NVIC线路使能
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2;	//低于 USB(0) 与 USART1(1)，与 TIM3 同级
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;			//子优先级 0
	NVIC_Init(&NVIC_InitStructure);								//将结构体变量交给NVIC_Init，配置NVIC外设
	
	/*TIM使能*/
	TIM_Cmd(TIM2, ENABLE);			//使能TIM2，定时器开始运行
}

void TIM3_Init(u16 arr,u16 psc)
{
	
	NVIC_InitTypeDef NVIC_InitStructure; 	
		// 设置中断来源
    NVIC_InitStructure.NVIC_IRQChannel = TIM3_IRQn ;	
		// 抢占优先级与 TIM2 保持同级（2），避免抢占冲突
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2;	 
	  // 设置子优先级为 1
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;	
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
	
		TIM_TimeBaseInitTypeDef  TIM_TimeBaseStructure;		
		// 开启定时器时钟,即内部时钟CK_INT=72M
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);	
		// 自动重装载寄存器的值，累计TIM_Period+1个频率后产生一个更新或者中断
    TIM_TimeBaseStructure.TIM_Period=arr;
	  // 时钟预分频数
    TIM_TimeBaseStructure.TIM_Prescaler= psc;	
		// 时钟分频因子 ，没用到不用管
    TIM_TimeBaseStructure.TIM_ClockDivision=TIM_CKD_DIV1;		
		// 计数器计数模式，设置为向上计数
    TIM_TimeBaseStructure.TIM_CounterMode=TIM_CounterMode_Up; 		
		// 重复计数器的值，没用到不用管
		TIM_TimeBaseStructure.TIM_RepetitionCounter=0;	
	  // 初始化定时器
    TIM_TimeBaseInit(TIM3, &TIM_TimeBaseStructure);
	
		// 清除计数器中断标志位
    TIM_ClearFlag(TIM3, TIM_FLAG_Update);
	  
		// 开启计数器中断
    TIM_ITConfig(TIM3,TIM_IT_Update,ENABLE);
		
		// 使能计数器
    TIM_Cmd(TIM3, ENABLE);
}

void TIM3_IRQHandler(void) 
{
	if ( TIM_GetITStatus( TIM3, TIM_IT_Update) != RESET ) 
	{	
		TIM_ClearITPendingBit(TIM3 , TIM_FLAG_Update); 
		fall_tick_50ms_flag = 1;     // ADXL345 采样交由主循环执行，避免中断与主循环抢占 SysTick/I2C
		Fall_SOS_LED_Tick_50ms();    // 求救状态 LED 快闪在中断内稳态运行
	}		 	
}


