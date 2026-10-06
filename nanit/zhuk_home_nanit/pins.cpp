// Піни макета за схемою ud_v3.1. Частина пінів залежить від версії плати,
// яку NanitLib визначає під час роботи, тому таблиця заповнюється в setup().
#include <NanitLib.h>
#include "zh.h"

Pins pins;
uint8_t portPins[12][4];  // усі сигнальні піни портів, для команди SCAN

void pinsBegin() {
  pins.v31 = getBoardVersion() >= Version(3, 1);

  // Порт 1: сервопривід штори й двигун вентилятора
  pins.servo = P1_1;
  pins.motA = MOTOR1_A;
  pins.motB = MOTOR1_B;
  pins.motEn = MOTOR_ENABLE;
  // Порт 2: кроковий двигун шлагбаума через ULN2003
  pins.gate[0] = P2_1;
  pins.gate[1] = P2_2;
  pins.gate[2] = P2_3;
  pins.gate[3] = P2_4;
  // Порт 3: світлофор
  pins.tlR = P3_2;
  pins.tlY = P3_3;
  pins.tlG = P3_4;
  // Порт 4: RGB-світло
  pins.rgb[0] = P4_2;
  pins.rgb[1] = P4_3;
  pins.rgb[2] = P4_4;
  // Порт 5: рух, звук, зумер, датчик вікна
  pins.pir = P5_1;
  pins.sound = P5_2;
  pins.buzzer = P5_3;
  pins.wsens = P5_4;
  // Порт 6: DHT11, MQ-7, освітленість
  pins.dht = P6_1;
  pins.mq7 = P6_2;
  pins.ldr = P6_3;
  // Порти 7 і 10: клавіатура, рядки R1–R4 і стовпці C1–C4
  pins.kpRow[0] = P7_4;
  pins.kpRow[1] = P7_3;
  pins.kpRow[2] = P7_2;
  pins.kpRow[3] = P7_1;
  pins.kpCol[0] = P10_1;
  pins.kpCol[1] = P10_2;
  pins.kpCol[2] = P10_3;
  pins.kpCol[3] = P10_4;
  // 0xFF — пін, недоступний у стандартному ядрі Arduino (P3_1) або пін скидання (P8_4)
  const uint8_t all[12][4] = {
      {P1_1, P1_2, P1_3, P1_4},     {P2_1, P2_2, P2_3, P2_4},
      {0xFF, P3_2, P3_3, P3_4},     {P4_1, P4_2, P4_3, P4_4},
      {P5_1, P5_2, P5_3, P5_4},     {P6_1, P6_2, P6_3, P6_4},
      {P7_1, P7_2, P7_3, P7_4},     {P8_1, P8_2, P8_3, 0xFF},
      {P9_1, P9_2, P9_3, P9_4},     {P10_1, P10_2, P10_3, P10_4},
      {P11_1, P11_2, P11_3, P11_4}, {P12_1, P12_2, P12_3, P12_4}};
  memcpy(portPins, all, sizeof(portPins));

  // Порт 9: дисплей TM1637 і далекомір HC-SR04
  pins.tmClk = P9_1;
  pins.tmDio = P9_2;
  pins.trig = P9_3;
  pins.echo = P9_4;
}
