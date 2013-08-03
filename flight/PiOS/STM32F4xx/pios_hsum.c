/**
 ******************************************************************************
 * @addtogroup PIOS PIOS Core hardware abstraction layer
 * @{
 * @addtogroup PIOS_HSUM Graupner HoTT receiver functions
 * @{
 *
 * @file       pios_hsum.c
 * @author     ernie of infect
 * @brief      Graupner HoTT receiver functions for SUMD/H
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/* Project Includes */
#include "pios_hsum_priv.h"

#if defined(PIOS_INCLUDE_HSUM)

#if !defined(PIOS_INCLUDE_RTC)
#error PIOS_INCLUDE_RTC must be used to use HSUM
#endif

/* Forward Declarations */
static int32_t PIOS_HSUM_Get(uintptr_t rcvr_id, uint8_t channel);
static uint16_t PIOS_HSUM_RxInCallback(uintptr_t context,
                                       uint8_t *buf,
                                       uint16_t buf_len,
                                       uint16_t *headroom,
                                       bool *need_yield);
static void PIOS_HSUM_Supervisor(uintptr_t hsum_id);

/* Local Variables */
const struct pios_rcvr_driver pios_hsum_rcvr_driver = {
	.read = PIOS_HSUM_Get,
};

enum pios_hsum_dev_magic {
    PIOS_HSUM_DEV_MAGIC = 0x4853554D,
};

struct pios_hsum_state {
	uint16_t channel_data[PIOS_HSUM_NUM_INPUTS];
	uint8_t received_data[HSUM_MAX_FRAME_LENGTH];
	uint8_t receive_timer;
	uint8_t failsafe_timer;
	uint8_t frame_found;
	uint8_t byte_count;
	uint8_t frame_length;
};

struct pios_hsum_dev {
	enum pios_hsum_dev_magic magic;
	const struct pios_hsum_cfg *cfg;
	enum pios_hsum_proto proto;
	struct pios_hsum_state state;
};

/* Allocate HSUM device descriptor */
#if defined(PIOS_INCLUDE_FREERTOS)
static struct pios_hsum_dev *PIOS_HSUM_Alloc(void)
{
	struct pios_hsum_dev *hsum_dev;

	hsum_dev = (struct pios_hsum_dev *)pvPortMalloc(sizeof(*hsum_dev));
	if (!hsum_dev)
		return NULL;

	hsum_dev->magic = PIOS_HSUM_DEV_MAGIC;
	return hsum_dev;
}
#else
static struct pios_hsum_dev pios_hsum_devs[PIOS_HSUM_MAX_DEVS];
static uint8_t pios_hsum_num_devs;
static struct pios_hsum_dev *PIOS_HSUM_Alloc(void)
{
	struct pios_hsum_dev *hsum_dev;

	if (pios_hsum_num_devs >= PIOS_HSUM_MAX_DEVS)
		return NULL;

	hsum_dev = &pios_hsum_devs[pios_hsum_num_devs++];
	hsum_dev->magic = PIOS_HSUM_DEV_MAGIC;

	return hsum_dev;
}
#endif

/* Validate HSUM device descriptor */
static bool PIOS_HSUM_Validate(struct pios_hsum_dev *hsum_dev)
{
	return (hsum_dev->magic == PIOS_HSUM_DEV_MAGIC);
}

/* Reset channels in case of lost signal or explicit failsafe receiver flag */
static void PIOS_HSUM_ResetChannels(struct pios_hsum_dev *hsum_dev)
{
	struct pios_hsum_state *state = &(hsum_dev->state);
	for (int i = 0; i < PIOS_HSUM_NUM_INPUTS; i++) {
		state->channel_data[i] = PIOS_RCVR_TIMEOUT;
	}
}

/* Reset HSUM receiver state */
static void PIOS_HSUM_ResetState(struct pios_hsum_dev *hsum_dev)
{
	struct pios_hsum_state *state = &(hsum_dev->state);
	state->receive_timer = 0;
	state->failsafe_timer = 0;
	state->frame_found = 0;
	PIOS_HSUM_ResetChannels(hsum_dev);
}

/**
 * Check and unroll complete frame data.
 * \output 0 frame data accepted
 * \output -1 frame error found
 */
static int PIOS_HSUM_UnrollChannels(struct pios_hsum_dev *hsum_dev)
{
	struct pios_hsum_state *state = &(hsum_dev->state);

	/* check the header and crc for a valid HoTT SUM stream */
	uint8_t vendor = state->received_data[0];
	uint8_t status = state->received_data[1];
	if (vendor != 0xA8)
		/* Graupner ID was expected */
		goto stream_error;

	switch (status) {
	case 0x00:
	case 0x01:
	case 0x81:
		/* check crc before processing */
		if (hsum_dev->proto == PIOS_HSUM_PROTO_SUMD) {
			/* SUMD has 16 bit CCITT CRC */
			uint16_t crc = 0;
			uint8_t *s = &(state->received_data[0]);
			int len = state->byte_count - 2;
			for (int n = 0; n < len; n++) {
				crc ^= (uint16_t)s[n] << 8;
				for (int i = 0; i < 8; i++)
					crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
			}
			if (crc ^ (((uint16_t)s[len] << 8) | s[len + 1]))
				/* wrong crc checksum found */
				goto stream_error;
		}
		if (hsum_dev->proto == PIOS_HSUM_PROTO_SUMH) {
			/* SUMH has only 8 bit added CRC */
			uint8_t crc = 0;
			uint8_t *s = &(state->received_data[0]);
			int len = state->byte_count - 1;
			for (int n = 0; n < len; n++)
				crc += s[n];
			if (crc ^ s[len])
				/* wrong crc checksum found */
				goto stream_error;
		}
		break;
	default:
		/* wrong header format */
		goto stream_error;
	}

	/* unroll channels */
	uint8_t n_channels = state->received_data[2];
	uint8_t *s = &(state->received_data[3]);
	uint16_t word;

	for (int i = 0; i < HSUM_MAX_CHANNELS_PER_FRAME; i++) {
		if (i < n_channels) {
			word = ((uint16_t)s[0] << 8) | s[1];
			s += 2;
			/* save the channel value */
			if (i < PIOS_HSUM_NUM_INPUTS) {
				/* floating version. channel limits from -100..+100% are mapped to 1000..2000 */
				state->channel_data[i] = (uint16_t)(word / 6.4 - 375);
			}
		} else
			/* this channel was not received */
			state->channel_data[i] = PIOS_RCVR_INVALID;
	}

	/* all channels processed */
	return 0;

stream_error:
	/* either SUMD selected with SUMH stream found, or vice-versa */
	return -1;
}

/* Update decoder state processing input byte from the HoTT stream */
static void PIOS_HSUM_UpdateState(struct pios_hsum_dev *hsum_dev, uint8_t byte)
{
	struct pios_hsum_state *state = &(hsum_dev->state);
	if (state->frame_found) {
		/* receiving the data frame */
		if (state->byte_count < HSUM_MAX_FRAME_LENGTH) {
			/* store next byte */
			state->received_data[state->byte_count++] = byte;
			if (state->byte_count == 3) {
				/* 3rd byte contains the number of channels. calculate frame size */
				state->frame_length = 5 + 2 * byte;
			}
			if (state->byte_count == state->frame_length) {
				/* full frame received - process and wait for new one */
				if (!PIOS_HSUM_UnrollChannels(hsum_dev))
					/* data looking good */
					state->failsafe_timer = 0;
				/* prepare for the next frame */
				state->frame_found = 0;
			}
		}
	}
}

/* Initialise HoTT receiver interface */
int32_t PIOS_HSUM_Init(uintptr_t *hsum_id,
                       const struct pios_hsum_cfg *cfg,
                       const struct pios_com_driver *driver,
                       uintptr_t lower_id,
                       enum pios_hsum_proto proto)
{
	PIOS_DEBUG_Assert(hsum_id);
	PIOS_DEBUG_Assert(cfg);
	PIOS_DEBUG_Assert(driver);

	struct pios_hsum_dev *hsum_dev;

	hsum_dev = (struct pios_hsum_dev *)PIOS_HSUM_Alloc();
	if (!hsum_dev)
		return -1;

	/* Bind the configuration to the device instance */
	hsum_dev->cfg = cfg;
	hsum_dev->proto = proto;

	PIOS_HSUM_ResetState(hsum_dev);

	*hsum_id = (uintptr_t)hsum_dev;

	/* Set comm driver callback */
	(driver->bind_rx_cb)(lower_id, PIOS_HSUM_RxInCallback, *hsum_id);

	if (!PIOS_RTC_RegisterTickCallback(PIOS_HSUM_Supervisor, *hsum_id)) {
		PIOS_DEBUG_Assert(0);
	}

	return 0;
}

/* Comm byte received callback */
static uint16_t PIOS_HSUM_RxInCallback(uintptr_t context,
                                       uint8_t *buf,
                                       uint16_t buf_len,
                                       uint16_t *headroom,
                                       bool *need_yield)
{
	struct pios_hsum_dev *hsum_dev = (struct pios_hsum_dev *)context;

	bool valid = PIOS_HSUM_Validate(hsum_dev);
	PIOS_Assert(valid);

	/* process byte(s) and clear receive timer */
	for (uint8_t i = 0; i < buf_len; i++) {
		PIOS_HSUM_UpdateState(hsum_dev, buf[i]);
		hsum_dev->state.receive_timer = 0;
	}

	/* Always signal that we can accept another byte */
	if (headroom)
		*headroom = HSUM_MAX_FRAME_LENGTH;

	/* We never need a yield */
	*need_yield = false;

	/* Always indicate that all bytes were consumed */
	return buf_len;
}

/**
 * Get the value of an input channel
 * \param[in] channel Number of the channel desired (zero based)
 * \output PIOS_RCVR_INVALID channel not available
 * \output PIOS_RCVR_TIMEOUT failsafe condition or missing receiver
 * \output >=0 channel value
 */
static int32_t PIOS_HSUM_Get(uintptr_t rcvr_id, uint8_t channel)
{
	struct pios_hsum_dev *hsum_dev = (struct pios_hsum_dev *)rcvr_id;

	if (!PIOS_HSUM_Validate(hsum_dev))
		return PIOS_RCVR_INVALID;

	/* return error if channel is not available */
	if (channel >= PIOS_HSUM_NUM_INPUTS)
		return PIOS_RCVR_INVALID;

	/* may also be PIOS_RCVR_TIMEOUT set by other function */
	return hsum_dev->state.channel_data[channel];
}

/**
 * Input data supervisor is called periodically and provides
 * two functions: frame syncing and failsafe triggering.
 *
 * HSUM frames come at 11ms or 22ms rate at 115200bps.
 * RTC timer is running at 625Hz (1.6ms). So with divider 5 it gives
 * 8ms pause between frames which is good for both HSUM frame rates.
 *
 * Data receive function must clear the receive_timer to confirm new
 * data reception. If no new data received in 100ms, we must call the
 * failsafe function which clears all channels.
 */
static void PIOS_HSUM_Supervisor(uintptr_t hsum_id)
{
	struct pios_hsum_dev *hsum_dev = (struct pios_hsum_dev *)hsum_id;

	bool valid = PIOS_HSUM_Validate(hsum_dev);
	PIOS_Assert(valid);

	struct pios_hsum_state *state = &(hsum_dev->state);

	/* waiting for new frame if no bytes were received in 8ms */
	if (++state->receive_timer > 4) {
		state->frame_found = 1;
		state->byte_count = 0;
		state->receive_timer = 0;
		state->frame_length = 3;
	}

	/* activate failsafe if no frames have arrived in 102.4ms */
	if (++state->failsafe_timer > 64) {
		PIOS_HSUM_ResetChannels(hsum_dev);
		state->failsafe_timer = 0;
	}
}

#endif	/* PIOS_INCLUDE_HSUM */

/**
 * @}
 * @}
 */
