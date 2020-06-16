/*
 * fusbpd.cpp
 *
 *  Created on: 13 Jun 2020
 *      Author: Ralim
 */

#include <fusbpd.h>
#include <pd.h>
#include "I2CBB.hpp"
#include "fusb302b.h"
#include "policy_engine.h"
#include "protocol_rx.h"
#include "protocol_tx.h"
#include "int_n.h"
#include "hard_reset.h"

uint8_t fusb302_detect() {
	//Probe the I2C bus for its address
	return I2CBB::probe(FUSB302B_ADDR);
}

void fusb302_start_processing() {
	HAL_Delay(30);
	/* Initialize the FUSB302B */
	fusb_setup();

	/* Create the policy engine thread. */
	PolicyEngine::init();

	/* Create the protocol layer threads. */
	ProtocolReceive::init();
	ProtocolTransmit::init();
	ResetHandler::init();

	/* Create the INT_N thread. */
	InterruptHandler::init();
}
