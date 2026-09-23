#define DCT_CON GPIO_PIN_14     // PC14 电磁阀控制 输出 已完成
#define KEY GPIO_PIN_15         // PC15 按键检测 输入 (目前已完成发送到TUYA端，TUYA端串口打印即可暂时不用DP响应APP)
#define USB_DET GPIO_PIN_5      // PF5 USB插入检测 输入 (发送到TUYA端，TUYA端串口打印即可暂时不用DP响应APP)
#define NTC_CON GPIO_PIN_1      // PB1 NTC AD采样电源开关控制 输出 常开 待实现
#define CHARGE_EN GPIO_PIN_4    // PB4 充电使能 输出 常开 （默认常开，由NTC温度检测控制是否需要断开或恢复）
#define CHARGE_STATE GPIO_PIN_5 // PB5 充电状态检测 输入 (发送到TUYA端，TUYA端串口打印即可暂时不用DP响应APP)
#define LED_G GPIO_PIN_6        // PB6 充电指示绿灯 输出  (90%以上电量常亮)
#define LED_R GPIO_PIN_7        // PB7 充电指示红灯 输出  (充电中常亮， 电量低于20% 2hz闪烁)
#define UART_TX GPIO_PIN_8      // PB8 串口输出 (根据是否需要串口协议进行控制实现继续开发即可)
#define UART_RX GPIO_PIN_9      // PB9 串口输入 (根据是否需要串口协议进行控制实现继续开发即可)
#define MOTOR_PWM GPIO_PIN_2    // PA2 电机PWM控制 占空比3个挡位控制电机 输出 TIM2_CH3 已完成
#define LED_LOW GPIO_PIN_3      // PA3 低档位指示灯 输出 低电平亮 高电平灭 （根据当前发球距离模式点亮对应灯，TUYA端开机时对应常亮，TUYA端关机时熄灭，TUYA端休眠时闪烁）
#define LED_MIDDLE GPIO_PIN_4   // PA4 中档位指示灯 输出 低电平亮 高电平灭 （根据当前发球距离模式点亮对应灯，TUYA端开机时对应常亮，TUYA端关机时熄灭，TUYA端休眠时闪烁）
#define LED_HIGH GPIO_PIN_5     // PA5 高档位指示灯 输出 低电平亮 高电平灭 （根据当前发球距离模式点亮对应灯，TUYA端开机时对应常亮，TUYA端关机时熄灭，TUYA端休眠时闪烁）
#define IE_PWM GPIO_PIN_8       // PA8 对射装置发射IO 38kHz的50%占空比，3ms有，3ms无，一直持续 TIM1_CH1 待实现
#define LEIDA_IN_O GPIO_PIN_10  // PA10 充电使能 雷达检测人是否存在 输入 待实现
#define IR GPIO_PIN_11          // PA11 对射装置检测IO 输入 (有球在腔里会变成高电平，无球时低电平)
#define IR_CON GPIO_PIN_12      // P12 对射装置开关IO 直接输出低电平 暂时不用

#define AD_NTC GPIO_PIN_0     // PB0 NTC AD采样 (NTC温度控制)
#define AD_I_SHUNT GPIO_PIN_1 // PA1 暂时不用实现这个AD采样 
#define AD_BAT GPIO_PIN_7     // PA7 电池电压 AD采样 待实现(3.7V*2=7.4V电池，写一个电池电量软件算法进行百分比告知TUYA端，TUYA端DP通知APP)
