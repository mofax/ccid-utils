/*
 * This file is part of cci-utils
 * Copyright (c) 2008 Gianni Tedesco <gianni@scaramanga.co.uk>
 * Released under the terms of the GNU GPL version 3
 *
 * Interface to a chipcard interface slot.
*/

#include <ccid.h>

#include <stdio.h>

#include "ccid-internal.h"

unsigned int chipcard_status(chipcard_t cc)
{
	return cc->cc_status;
}

unsigned int chipcard_slot_status(chipcard_t cc)
{
	struct _cci *cci = cc->cc_parent;

	if ( !_PC_to_RDR_GetSlotStatus(cci, cc->cc_idx, cci->cci_xfr) )
		return CHIPCARD_CLOCK_ERR;

	if ( !_RDR_to_PC(cci, cc->cc_idx, cci->cci_xfr) )
		return CHIPCARD_CLOCK_ERR;

	return _RDR_to_PC_SlotStatus(cci, cci->cci_xfr);
}

int chipcard_slot_on(chipcard_t cc, unsigned int voltage)
{
	struct _cci *cci = cc->cc_parent;

	if ( !_PC_to_RDR_IccPowerOn(cci, cc->cc_idx, cci->cci_xfr, voltage) )
		return 0;

	if ( !_RDR_to_PC(cci, cc->cc_idx, cci->cci_xfr) )
		return 0;
	
	_RDR_to_PC_DataBlock(cci, cci->cci_xfr);
	return 1;
}

int chipcard_transact(chipcard_t cc, xfr_t xfr)
{
	struct _cci *cci = cc->cc_parent;

	if ( !_PC_to_RDR_XfrBlock(cci, cc->cc_idx, xfr) )
		return 0;

	if ( !_RDR_to_PC(cci, cc->cc_idx, xfr) )
		return 0;

	_RDR_to_PC_DataBlock(cci, xfr);
	return 1;
}

int chipcard_slot_off(chipcard_t cc)
{
	struct _cci *cci = cc->cc_parent;

	if ( !_PC_to_RDR_IccPowerOff(cci, cc->cc_idx, cci->cci_xfr) )
		return 0;

	if ( !_RDR_to_PC(cci, cc->cc_idx, cci->cci_xfr) )
		return 0;
	
	_RDR_to_PC_SlotStatus(cci, cci->cci_xfr);

	return 1;
}

int chipcard_wait_for_card(chipcard_t cc)
{
	while( cc->cc_status == CHIPCARD_NOT_PRESENT )
		if ( !_cci_wait_for_interrupt(cc->cc_parent) )
			return 0;
	return 1;
}

cci_t chipcard_cci(chipcard_t cc)
{
	return cc->cc_parent;
}
