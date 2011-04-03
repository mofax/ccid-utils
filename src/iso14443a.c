#include <ccid.h>
#include <unistd.h>

#include "ccid-internal.h"
#include "iso14443a.h"

/* ISO 14443-3, Chapter 6.3.1 */
#define ISO14443A_SF_CMD_REQA 		0x26
#define ISO14443A_SF_CMD_WUPA 		0x52
#define ISO14443A_SF_CMD_OPT_TIMESLOT	0x35 /* Annex C */
/* 40 to 4f and 78 to 7f: proprietary */

#define	ISO14443A_BITOFCOL_NONE		0xffffffff

enum rfid_frametype {
	RFID_14443A_FRAME_REGULAR,
	RFID_14443B_FRAME_REGULAR,
	RFID_MIFARE_FRAME,
	RFID_15693_FRAME,
	RFID_15693_FRAME_ICODE1,
};

enum rfid_layer2_id {
	RFID_LAYER2_NONE,
	RFID_LAYER2_ISO14443A,
	RFID_LAYER2_ISO14443B,
	RFID_LAYER2_ISO15693,
	RFID_LAYER2_ICODE1,
};

enum iso14443a_level {
	ISO14443A_LEVEL_NONE,
	ISO14443A_LEVEL_CL1,
	ISO14443A_LEVEL_CL2,
	ISO14443A_LEVEL_CL3,
};

enum iso14443a_state {
	ISO14443A_STATE_ERROR,
	ISO14443A_STATE_NONE,
	ISO14443A_STATE_REQA_SENT,
	ISO14443A_STATE_ATQA_RCVD,
	ISO14443A_STATE_NO_BITFRAME_ANTICOL,
	ISO14443A_STATE_ANTICOL_RUNNING,
	ISO14443A_STATE_SELECTED,
};

enum rfid_protocol_id {
	RFID_PROTOCOL_UNKNOWN,
	RFID_PROTOCOL_TCL,
	RFID_PROTOCOL_MIFARE_UL,
	RFID_PROTOCOL_MIFARE_CLASSIC,
	RFID_PROTOCOL_ICODE_SLI,
	RFID_PROTOCOL_TAGIT,
	NUM_RFID_PROTOCOLS
};

#define TIMEOUT 1236

/* transceive anti collission bitframe */
int _iso14443a_transceive_acf(struct _cci *cci,
					struct iso14443a_anticol_cmd *acf,
					unsigned int *bit_of_col)
{
	uint8_t rx_buf[64];
	uint8_t rx_len = sizeof(rx_buf);
	uint8_t rx_align = 0, tx_last_bits, tx_bytes, tx_bytes_total;
	uint8_t boc;
	uint8_t error_flag;
	struct rf_mode mode = {
		.flags = RF_PARITY_ENABLE,
	};

	*bit_of_col = ISO14443A_BITOFCOL_NONE;
	memset(rx_buf, 0, sizeof(rx_buf));

	tx_last_bits = acf->nvb & 0x07;	/* lower nibble indicates bits */
	tx_bytes = ( acf->nvb >> 4 ) & 0x07;
	if (tx_last_bits) {
		tx_bytes_total = tx_bytes+1;
		rx_align = tx_last_bits & 0x07; /* rx frame complements tx */
	}else{
		tx_bytes_total = tx_bytes;
	}

	mode.rx_align = rx_align;
	mode.tx_last_bits = tx_last_bits;
	if ( !_clrc632_set_rf_mode(cci, &mode) )
		return 0;

	if ( !_clrc632_transceive(cci, (uint8_t *)acf, tx_bytes_total,
				rx_buf, &rx_len, 0x32, 0) )
		return 0;

	/* bitwise-OR the two halves of the split byte */
	acf->uid_bits[tx_bytes-2] = (
		  (acf->uid_bits[tx_bytes-2] & (0xff >> (8-tx_last_bits)))
		| rx_buf[0]);

	/* copy the rest */
	if (rx_len)
		memcpy(&acf->uid_bits[tx_bytes-1], &rx_buf[1], rx_len-1);

	/* determine whether there was a collission */
	if ( !_clrc632_get_error(cci, &error_flag) )
		return 0;

	if (error_flag & RF_ERR_COLLISION ) {
		/* retrieve bit of collission */
		if ( !_clrc632_get_coll_pos(cci, &boc) )
			return 0;

		/* bit of collission relative to start of part 1 of
		 * anticollision frame (!) */
		*bit_of_col = 2*8 + boc;
	}

	return 1;
}

/* transceive regular frame */
int _iso14443ab_transceive(struct _cci *cci,
				   unsigned int frametype,
				   const uint8_t *tx_buf, unsigned int tx_len,
				   uint8_t *rx_buf, unsigned int *rx_len,
				   uint64_t timeout, unsigned int flags)
{
	int ret;
	uint8_t rxl;
	struct rf_mode mode = {
		.tx_last_bits = 0,
	};

	if (*rx_len > 0xff)
		rxl = 0xff;
	else
		rxl = *rx_len;

	memset(rx_buf, 0, *rx_len);

	switch (frametype) {
	case RFID_14443A_FRAME_REGULAR:
	case RFID_MIFARE_FRAME:
		mode.flags = RF_PARITY_ENABLE | RF_TX_CRC | RF_RX_CRC;
		break;
	case RFID_14443B_FRAME_REGULAR:
		mode.flags = RF_TX_CRC | RF_RX_CRC /* | 3309 */;
		break;
#if 0
	case RFID_MIFARE_FRAME:
		mode.flags = RF_PARITY_ENABLE;
		break;
#endif
	case RFID_15693_FRAME:
		mode.flags = RF_TX_CRC | RF_RX_CRC /* | 3309 */;
		break;
	case RFID_15693_FRAME_ICODE1:
		/* FIXME: implement */
	default:
		return 0;
	}

	if ( !_clrc632_set_rf_mode(cci, &mode) )
		return 0;

	ret = _clrc632_transceive(cci, tx_buf, tx_len,
					rx_buf, &rxl, timeout, 0);
	*rx_len = rxl;
	if (!ret)
		return 0;

	return 1;
}

/* issue a 14443-3 A PCD -> PICC command in a short frame, such as REQA, WUPA */
int _iso14443a_transceive_sf(struct _cci *cci,
					uint8_t cmd,
		    			struct iso14443a_atqa *atqa)
{
	uint8_t tx_buf[1];
	uint8_t rx_len = 2;
	uint8_t error_flag;
	static const struct rf_mode mode = {
		.tx_last_bits = 7,
		.flags = RF_PARITY_ENABLE,
	};

	if ( !_clrc632_set_rf_mode(cci, &mode) )
		return 0;

	memset(atqa, 0, sizeof(*atqa));

	tx_buf[0] = cmd;

	if ( !_clrc632_transceive(cci, tx_buf, sizeof(tx_buf),
				(uint8_t *)atqa, &rx_len,
				ISO14443A_FDT_ANTICOL_LAST1, 0) ) {
		printf("error during rc632_transceive()\n");
		return 0;
	}

	/* determine whether there was a collission */
	if ( !_clrc632_get_error(cci, &error_flag) )
		return 0;

	if (error_flag & RF_ERR_COLLISION ) {
		uint8_t boc;

		/* retrieve bit of collission */
		if ( !_clrc632_get_coll_pos(cci, &boc) )
			return 0;

		printf("collision detected in xcv_sf: bit_of_col=%u\n", boc);
		/* FIXME: how to signal this up the stack */
	}

	if (rx_len != 2) {
		printf("rx_len(%d) != 2\n", rx_len);
		return 0;
	}

	return 1;
}

static int random_bit(void)
{
	static unsigned long randctx[4] = {0x22d4a017,
						0x773a1f44,
						0xc39e1460,
						0x9cde8801};
	unsigned long e;

	e = randctx[0];
	randctx[0] = randctx[1];
	randctx[1] = (randctx[2]<<19) + (randctx[2]>>13) + randctx[3];
	randctx[2] = randctx[3] ^ randctx[0];
	randctx[3] = e+randctx[1];

	return randctx[1]&1;
}

/* first bit is '1', second bit '2' */
static void
rnd_toggle_bit_in_field(unsigned char *bitfield, unsigned int size, unsigned int bit)
{
	unsigned int byte,rnd;

	if (bit && (bit <= (size*8))) {
		rnd = random_bit();

		printf("xor'ing bit %u with %u\n",bit,rnd);
		bit--;
		byte = bit/8;
		bit = rnd << (bit % 8);
		bitfield[byte] ^= bit;
	}
}


static int
iso14443a_code_nvb_bits(unsigned char *nvb, unsigned int bits)
{
	unsigned int byte_count = bits / 8;
	unsigned int bit_count = bits % 8;

	if (byte_count < 2 || byte_count > 7)
		return -1;

	*nvb = ((byte_count & 0xf) << 4) | bit_count;

	return 0;
}

int _iso14443a_select(struct _cci *cci, int wup)
{
	int ret;
	unsigned int uid_size;
	struct iso14443a_atqa atqa;
	struct iso14443a_anticol_cmd acf;
	unsigned int bit_of_col;
	unsigned char sak[3];
	uint8_t uid[10];
	size_t uid_len;
	unsigned int rx_len = sizeof(sak);
	char *aqptr = (char *)&atqa;
	unsigned int state, level, tcl_capable, proto_supported;

	memset(uid, 0, sizeof(uid));
	memset(sak, 0, sizeof(sak));
	memset(&atqa, 0, sizeof(atqa));
	memset(&acf, 0, sizeof(acf));
	uid_len = 0;
	tcl_capable = proto_supported = 0;

	state = ISO14443A_STATE_NONE;
	level = ISO14443A_LEVEL_NONE;

	if (wup) {
		printf("Sending WUPA\n");
		ret = _iso14443a_transceive_sf(cci,
					ISO14443A_SF_CMD_WUPA, &atqa);
	}else{
		printf("Sending REQA\n");
		ret = _iso14443a_transceive_sf(cci,
					ISO14443A_SF_CMD_REQA, &atqa);
	}

	if (!ret) {
		state = ISO14443A_STATE_REQA_SENT;
		printf("error during transceive_sf: %d\n", ret);
		return 0;
	}
	state = ISO14443A_STATE_ATQA_RCVD;

	printf("ATQA: 0x%02x 0x%02x\n", *aqptr, *(aqptr+1));

	if (!atqa.bf_anticol) {
		state = ISO14443A_STATE_NO_BITFRAME_ANTICOL;
		printf("no bitframe anticollission bits set, aborting\n");
		return -1;
	}
	printf("ATQA anticol bits = %d\n", atqa.bf_anticol);

	if (atqa.uid_size == 2 || atqa.uid_size == 3)
		uid_size = 3;
	else if (atqa.uid_size == 1)
		uid_size = 2;
	else
		uid_size = 1;

	printf("uid_size = %u\n", uid_size);
	acf.sel_code = ISO14443A_AC_SEL_CODE_CL1;

	state = ISO14443A_STATE_ANTICOL_RUNNING;
	level = ISO14443A_LEVEL_CL1;

cascade:
	rx_len = sizeof(sak);
	iso14443a_code_nvb_bits(&acf.nvb, 16);
	printf("ANTICOL: sel_code=%.2x nvb=%.2x\n", acf.sel_code, acf.nvb);

	ret = _iso14443a_transceive_acf(cci, &acf, &bit_of_col);
	printf("tran_acf->%d boc: %d\n",ret,bit_of_col);
	if (!ret)
		return 0;

	while (bit_of_col != ISO14443A_BITOFCOL_NONE) {
		printf("collision at pos %u\n", bit_of_col);

		iso14443a_code_nvb_bits(&acf.nvb, bit_of_col);
		rnd_toggle_bit_in_field(acf.uid_bits, sizeof(acf.uid_bits), bit_of_col);
		printf("acf: nvb=0x%02X uid_bits=...\n", acf.nvb);
		hex_dump(acf.uid_bits, sizeof(acf.uid_bits), 16);
		if ( !_iso14443a_transceive_acf(cci, &acf, &bit_of_col) )
			return 0;
	}

	iso14443a_code_nvb_bits(&acf.nvb, 7*8);

	printf("ANTICOL: sel_code=%.2x nvb=%.2x\n", acf.sel_code, acf.nvb);
	//hex_dump(acf.uid_bits, sizeof(acf.uid_bits), 16);
	hex_dump((uint8_t *)&acf, sizeof(acf), 16);
	if ( !_iso14443ab_transceive(cci, RFID_14443A_FRAME_REGULAR,
				   (unsigned char *)&acf, sizeof(acf),
				   (unsigned char *) &sak, &rx_len,
				   TIMEOUT, 0) )
		return 0;

	if (sak[0] & 0x04) {
		/* Cascade bit set, UID not complete */
		switch (acf.sel_code) {
		case ISO14443A_AC_SEL_CODE_CL1:
			/* cascading from CL1 to CL2 */
			printf("cascading from CL1 to CL2\n");
			if (acf.uid_bits[0] != 0x88) {
				printf("Cascade bit set, but UID0 != 0x88\n");
				return 0;
			}
			memcpy(&uid[0], &acf.uid_bits[1], 3);
			acf.sel_code = ISO14443A_AC_SEL_CODE_CL2;
			level = ISO14443A_LEVEL_CL2;
			break;
		case ISO14443A_AC_SEL_CODE_CL2:
			/* cascading from CL2 to CL3 */
			printf("cascading from CL2 to CL3\n");
			memcpy(&uid[3], &acf.uid_bits[1], 3);
			acf.sel_code = ISO14443A_AC_SEL_CODE_CL3;
			level = ISO14443A_LEVEL_CL3;
			break;
		default:
			printf("cannot cascade any further than CL3\n");
			state = ISO14443A_STATE_ERROR;
			return 0;
		}
		goto cascade;

	} else {
		switch (acf.sel_code) {
		case ISO14443A_AC_SEL_CODE_CL1:
			/* single size UID (4 bytes) */
			memcpy(&uid[0], &acf.uid_bits[0], 4);
			break;
		case ISO14443A_AC_SEL_CODE_CL2:
			/* double size UID (7 bytes) */
			memcpy(&uid[3], &acf.uid_bits[0], 4);
			break;
		case ISO14443A_AC_SEL_CODE_CL3:
			/* triple size UID (10 bytes) */
			memcpy(&uid[6], &acf.uid_bits[0], 4);
			break;
		}
	}

	if (level == ISO14443A_LEVEL_CL1)
		uid_len = 4;
	else if (level == ISO14443A_LEVEL_CL2)
		uid_len = 7;
	else
		uid_len = 10;

	printf("UID: ...\n");
	hex_dump(uid, uid_len, 16);

	level = ISO14443A_LEVEL_NONE;
	state = ISO14443A_STATE_SELECTED;

	if (sak[0] & 0x20) {
		printf("we have a T=CL compliant PICC\n");
		proto_supported = 1 << RFID_PROTOCOL_TCL;
		tcl_capable = 1;
	} else {
		printf("we have a T!=CL PICC\n");
		proto_supported = (1 << RFID_PROTOCOL_MIFARE_UL)|
					  (1 << RFID_PROTOCOL_MIFARE_CLASSIC);
		tcl_capable = 0;
	}

	return 1;
}