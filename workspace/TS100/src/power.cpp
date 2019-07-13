/*
 * power.cpp
 *
 *  Created on: 28 Oct, 2018
 *     Authors: Ben V. Brown, David Hilton
 */

#include <power.hpp>
#include <Settings.h>
#include <hardware.h>

const uint16_t powerPWM = 255;
const uint16_t totalPWM = 255 + 30; //htim2.Init.Period, the full PWM cycle

// thermal mass = 1690 milliJ/*C for my tip.
//  ->  Wattsx10*Seconds to raise Temp from room temp to +100*C, divided by 100*C.
// we divide mass by 20 to let the I term dominate near the set point.
//  This is necessary because of the temp noise and thermal lag in the system.
// Once we have feed-forward temp estimation we should be able to better tune this.

#ifdef MODEL_TS100
const uint16_t tipMass = 2020 ; // divide here so division is compile-time.
const uint8_t tipResistance = 85;//x10 ohms, 8.5 typical for ts100, 4.5 typical for ts80

#endif
#ifdef MODEL_TS80
const uint16_t tipMass = 1000/4;
const uint8_t tipResistance = 46; //x10 ohms, 8.5 typical for ts100, 4.5 typical for ts80

#endif
history<uint32_t, oscillationPeriod> milliWattHistory = { { 0 }, 0, 0 };

int32_t tempToMilliWatts(int32_t rawTemp, uint8_t rawC) {
	// mass is in milliJ/*C, rawC is raw per degree C
	// returns milliWatts needed to raise/lower a mass by rawTemp
	//  degrees in one cycle.
	int32_t milliJoules = tipMass * (rawTemp / rawC);
	return milliJoules;
}

void setTipMilliWatts(int32_t mw) {
	//Enforce Max Watts Limiter # TODO

	int32_t output = milliWattsToPWM(mw, systemSettings.voltageDiv / 10, 1);
	setTipPWM(output);
	uint32_t actualMilliWatts = PWMToMilliWatts(output,
			systemSettings.voltageDiv / 10, 0);

	milliWattHistory.update(actualMilliWatts);
}

int32_t availableW10(uint8_t divisor, uint8_t sample) {
	//P = V^2 / R, v*v = v^2 * 100
	//				R = R*10
	// P therefore is in V^2*100/R*10 = W*10.
	int32_t v = getInputVoltageX10(divisor, sample);	// 100 = 10v
	int32_t availableWattsX10 = (v * v) / tipResistance;
	//However, 100% duty cycle is not possible as there is a dead time while the ADC takes a reading
	//Therefore need to scale available milliwats by this

	// avMw=(AvMw*powerPWM)/totalPWM.
	availableWattsX10 = availableWattsX10 * powerPWM;
	availableWattsX10 /= totalPWM;

	//availableMilliWattsX10 is now an accurate representation
	return availableWattsX10;
}

uint8_t milliWattsToPWM(int32_t milliWatts, uint8_t divisor, uint8_t sample) {

	// Scale input milliWatts to the pwm rate
	if (milliWatts < 10) // no pint driving tip
		return 0;

	//Calculate desired milliwatts as a percentage of availableW10
	int32_t pwm = (powerPWM * milliWatts) / availableW10(divisor, sample);
	if (pwm > powerPWM) {
		pwm = powerPWM;	//constrain to max PWM counter, shouldnt be possible, but small cost for safety to avoid wraps
	} else if (pwm < 0) {	//cannot go negative
		pwm = 0;
	}
	return pwm;
}

int32_t PWMToMilliWatts(uint8_t pwm, uint8_t divisor, uint8_t sample) {
	int32_t maxMW = availableW10(divisor, sample); //Get the milliwatts for the max pwm period
	//Then convert pwm into percentage of powerPWM to get the percentage of the max mw
	int32_t res = (pwm * maxMW) / powerPWM;
	if (res < 0)
		res = 0;
	return res;
}
