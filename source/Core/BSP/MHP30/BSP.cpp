// BSP mapping functions

#include "BSP.h"
#include "I2C_Wrapper.hpp"
#include "Model_Config.h"
#include "Pins.h"
#include "Setup.h"
#include "TipThermoModel.h"
#include "Utils.h"
#include "history.hpp"
#include "main.hpp"
#include <IRQ.h>
volatile uint16_t PWMSafetyTimer = 0;
volatile uint8_t  pendingPWM     = 0;
uint16_t          totalPWM       = 255;
const uint16_t    powerPWM       = 255;

history<uint16_t, PID_TIM_HZ> rawTempFilter = {{0}, 0, 0};
void                          resetWatchdog() { HAL_IWDG_Refresh(&hiwdg); }

#ifdef TEMP_NTC
// Lookup table for the NTC
// Stored as ADCReading,Temp in degC
static const uint16_t NTCHandleLookup[] = {
    // ADC Reading , Temp in C
    11292, 600, //
    12782, 550, //
    14380, 500, //
    16061, 450, //
    17793, 400, //
    19541, 350, //
    21261, 300, //
    22915, 250, //
    24465, 200, //
    25882, 150, //
    27146, 100, //
    28249, 50,  //
    29189, 0,   //
};
const int NTCHandleLookupItems = sizeof(NTCHandleLookup) / (2 * sizeof(uint16_t));
#endif

// These are called by the HAL after the corresponding events from the system
// timers.

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
  // Period has elapsed
  if (htim->Instance == TIM1) {
    // STM uses this for internal functions as a counter for timeouts
    HAL_IncTick();
  }
}
uint16_t getHandleTemperature() {
  int32_t result = getADC(0);
  return Utils::InterpolateLookupTable(NTCHandleLookup, NTCHandleLookupItems, result);
}

uint16_t getTipInstantTemperature() { return getADC(2); }

uint16_t getTipRawTemp(uint8_t refresh) {
  if (refresh) {
    uint16_t lastSample = getTipInstantTemperature();
    rawTempFilter.update(lastSample);
    return lastSample;
  } else {
    return rawTempFilter.average();
  }
}

uint16_t getInputVoltageX10(uint16_t divisor, uint8_t sample) {
  // ADC maximum is 32767 == 3.3V at input == 28.05V at VIN
  // Therefore we can divide down from there
  // Multiplying ADC max by 4 for additional calibration options,
  // ideal term is 467
  static uint8_t  preFillneeded = 10;
  static uint32_t samples[BATTFILTERDEPTH];
  static uint8_t  index = 0;
  if (preFillneeded) {
    for (uint8_t i = 0; i < BATTFILTERDEPTH; i++)
      samples[i] = getADC(1);
    preFillneeded--;
  }
  if (sample) {
    samples[index] = getADC(1);
    index          = (index + 1) % BATTFILTERDEPTH;
  }
  uint32_t sum = 0;

  for (uint8_t i = 0; i < BATTFILTERDEPTH; i++)
    sum += samples[i];

  sum /= BATTFILTERDEPTH;
  if (divisor == 0) {
    divisor = 1;
  }
  return sum * 4 / divisor;
}
bool tryBetterPWM(uint8_t pwm) {
  // We dont need this for the MHP30
  return false;
}
void setTipPWM(uint8_t pulse) {
  // We can just set the timer directly
  htim3.Instance->CCR1 = pulse;
}

void unstick_I2C() {
  GPIO_InitTypeDef GPIO_InitStruct;
  int              timeout     = 100;
  int              timeout_cnt = 0;

  // 1. Clear PE bit.
  hi2c1.Instance->CR1 &= ~(0x0001);
  /**I2C1 GPIO Configuration
   PB6     ------> I2C1_SCL
   PB7     ------> I2C1_SDA
   */
  //  2. Configure the SCL and SDA I/Os as General Purpose Output Open-Drain, High level (Write 1 to GPIOx_ODR).
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull  = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

  GPIO_InitStruct.Pin = SCL_Pin;
  HAL_GPIO_Init(SCL_GPIO_Port, &GPIO_InitStruct);
  HAL_GPIO_WritePin(SCL_GPIO_Port, SCL_Pin, GPIO_PIN_SET);

  GPIO_InitStruct.Pin = SDA_Pin;
  HAL_GPIO_Init(SDA_GPIO_Port, &GPIO_InitStruct);
  HAL_GPIO_WritePin(SDA_GPIO_Port, SDA_Pin, GPIO_PIN_SET);

  while (GPIO_PIN_SET != HAL_GPIO_ReadPin(SDA_GPIO_Port, SDA_Pin)) {
    // Move clock to release I2C
    HAL_GPIO_WritePin(SCL_GPIO_Port, SCL_Pin, GPIO_PIN_RESET);
    asm("nop");
    asm("nop");
    asm("nop");
    asm("nop");
    HAL_GPIO_WritePin(SCL_GPIO_Port, SCL_Pin, GPIO_PIN_SET);

    timeout_cnt++;
    if (timeout_cnt > timeout)
      return;
  }

  // 12. Configure the SCL and SDA I/Os as Alternate function Open-Drain.
  GPIO_InitStruct.Mode  = GPIO_MODE_AF_OD;
  GPIO_InitStruct.Pull  = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

  GPIO_InitStruct.Pin = SCL_Pin;
  HAL_GPIO_Init(SCL_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = SDA_Pin;
  HAL_GPIO_Init(SDA_GPIO_Port, &GPIO_InitStruct);

  HAL_GPIO_WritePin(SCL_GPIO_Port, SCL_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(SDA_GPIO_Port, SDA_Pin, GPIO_PIN_SET);

  // 13. Set SWRST bit in I2Cx_CR1 register.
  hi2c1.Instance->CR1 |= 0x8000;

  asm("nop");

  // 14. Clear SWRST bit in I2Cx_CR1 register.
  hi2c1.Instance->CR1 &= ~0x8000;

  asm("nop");

  // 15. Enable the I2C peripheral by setting the PE bit in I2Cx_CR1 register
  hi2c1.Instance->CR1 |= 0x0001;

  // Call initialization function.
  HAL_I2C_Init(&hi2c1);
}

uint8_t getButtonA() { return HAL_GPIO_ReadPin(KEY_A_GPIO_Port, KEY_A_Pin) == GPIO_PIN_RESET ? 1 : 0; }
uint8_t getButtonB() { return HAL_GPIO_ReadPin(KEY_B_GPIO_Port, KEY_B_Pin) == GPIO_PIN_RESET ? 1 : 0; }

void BSPInit(void) {}

void reboot() { NVIC_SystemReset(); }

void delay_ms(uint16_t count) { HAL_Delay(count); }

void setPlatePullup(bool pullingUp) {
  GPIO_InitTypeDef GPIO_InitStruct;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Pin   = PLATE_SENSOR_PULLUP_Pin;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  if (pullingUp) {
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_WritePin(PLATE_SENSOR_PULLUP_GPIO_Port, PLATE_SENSOR_PULLUP_Pin, GPIO_PIN_SET);
  } else {
    // Hi-z
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    HAL_GPIO_WritePin(PLATE_SENSOR_PULLUP_GPIO_Port, PLATE_SENSOR_PULLUP_Pin, GPIO_PIN_RESET);
  }
  HAL_GPIO_Init(PLATE_SENSOR_PULLUP_GPIO_Port, &GPIO_InitStruct);
}

uint16_t tipSenseResistancex10Ohms = 0;
bool     isTipDisconnected() {
  static bool       lastTipDisconnectedState = true;
  static uint16_t   adcReadingPD1Set         = 0;
  static TickType_t lastMeas                 = 0;
  // For the MHP30 we want to include a little extra logic in here
  // As when the tip is first connected we want to measure the ~100 ohm resistor on the base of the tip
  // And likewise if its removed we want to clear that measurement
  /*
   * plate_sensor_res = ((adc5_value_PD1_set - adc5_value_PD1_cleared) / (adc5_value_PD1_cleared + 4096 - adc5_value_PD1_set)) * 1000.0;
   * */

  uint16_t tipDisconnectedThres = TipThermoModel::getTipMaxInC() - 5;
  uint32_t tipTemp              = TipThermoModel::getTipInC();
  bool     tipDisconnected      = tipTemp > tipDisconnectedThres;
  // We have to handle here that this ^ will trip while measuring the gain resistor
  if (xTaskGetTickCount() - lastMeas < (TICKS_100MS * 2 + (TICKS_100MS / 2))) {
    tipDisconnected = false;
  }

  if (tipDisconnected != lastTipDisconnectedState) {
    if (tipDisconnected) {
      // Tip is now disconnected
      tipSenseResistancex10Ohms = 0; // zero out the resistance
      adcReadingPD1Set          = 0;
      lastMeas                  = 0;
    }
    lastTipDisconnectedState = tipDisconnected;
  }
  if (!tipDisconnected) {
    if (tipSenseResistancex10Ohms == 0) {
      if (lastMeas == 0) {
        lastMeas = xTaskGetTickCount();
        setPlatePullup(true);
      } else if (xTaskGetTickCount() - lastMeas > (TICKS_100MS)) {
        lastMeas = xTaskGetTickCount();
        // We are sensing the resistance
        if (adcReadingPD1Set == 0) {
          // We will record the reading for PD1 being set
          adcReadingPD1Set = getADC(3);
          setPlatePullup(false);
        } else {
          // We have taken reading one
          uint16_t adcReadingPD1Cleared = getADC(3);
          uint32_t a                    = ((int)adcReadingPD1Set - (int)adcReadingPD1Cleared);
          a *= 10000;
          uint32_t b = ((int)adcReadingPD1Cleared + (32768 - (int)adcReadingPD1Set));
          if (b) {
            tipSenseResistancex10Ohms = a / b;
          } else {
            tipSenseResistancex10Ohms = adcReadingPD1Set = lastMeas = 0;
          }
        }
      }
      return true; // we fake tip being disconnected until this is measured
    }
  }

  return tipDisconnected;
}
