#define DCT_CON GPIO_PIN_14     // PC14 电磁阀控制 输出
#define KEY GPIO_PIN_15         // PC15 按键检测 输入
#define USB_DET GPIO_PIN_5      // PF5 USB插入检测 输入
#define NTC_CON GPIO_PIN_1      // PB1 NTC AD采样电源开关控制 输出
#define CHARGE_EN GPIO_PIN_4    // PB4 充电使能 输出
#define CHARGE_STATE GPIO_PIN_5 // PB5 充电状态检测 输入
#define LED_G GPIO_PIN_6        // PB6 充电指示绿灯 输出
#define LED_R GPIO_PIN_7        // PB7 充电指示红灯 输出
#define UART_TX GPIO_PIN_8      // PB8 串口输出
#define UART_RX GPIO_PIN_9      // PB9 串口输入
#define MOTOR_PWM GPIO_PIN_2    // PA2 电机PWM控制 占空比3个挡位控制电机 输出
#define LED_LOW GPIO_PIN_3      // PA3 低档位指示灯 输出
#define LED_MIDDLE GPIO_PIN_4   // PA4 中档位指示灯 输出
#define LED_HIGH GPIO_PIN_5     // PA5 高档位指示灯 输出
#define IE_PWM GPIO_PIN_8       // PA8 对射装置发射IO 38kHz的50%占空比，3ms有，3ms无，一直持续
#define LEIDA_IN_O GPIO_PIN_10  // PA10 充电使能 雷达检测人是否存在 输入
#define IR GPIO_PIN_11          // PA11 充电使能 对射装置检测IO 输入
#define IR_CON GPIO_PIN_412     // P12 充电使能 直接输出低电平 暂时不用

#define AD_NTC GPIO_PIN_0     // PB0 NTC AD采样
#define AD_I_SHUNT GPIO_PIN_1 // PA1 暂时不用这个AD采样
#define AD_BAT GPIO_PIN_7     // PA7 电池电压 AD采样
