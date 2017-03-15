/*
 * BlueALSA - io-msbc.h
 * Copyright (c) 2017 Juha Kuikka
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_IO_MSBC_H_
#define BLUEALSA_IO_MSBC_H_

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <sbc/sbc.h>

#define SCO_H2_HDR_LEN		2
#define MSBC_FRAME_LEN		57
#define MSBC_PCM_LEN		240

struct msbc_frame {
	uint8_t h2_header[SCO_H2_HDR_LEN];
	uint8_t payload[MSBC_FRAME_LEN];
	uint8_t padding;
};

#define SCO_H2_FRAME_LEN	(sizeof(struct msbc_frame)) // TODO: revisit

struct sine_generator
{
	double samplerate;
	double freq;
	double step;
	double state;
};

struct sbc_state {
	size_t sbc_frame_len;

	/* decoder */
	sbc_t dec;
	size_t dec_buffer_cnt;
	size_t dec_buffer_size;
	uint8_t dec_buffer[SCO_H2_FRAME_LEN * 2];
	uint8_t dec_pcm_buffer[MSBC_PCM_LEN];

	/* encoder */
	sbc_t enc;
	size_t enc_buffer_cnt; /* bytes of data in the beginning of the buffer */
	size_t enc_buffer_size;
	uint8_t enc_buffer[6 * SCO_H2_FRAME_LEN];

	size_t enc_pcm_buffer_cnt; /* e.g. bytes of data in buffer */
	size_t enc_pcm_buffer_size; /* in bytes */
	uint8_t enc_pcm_buffer[MSBC_PCM_LEN * 5];
	ssize_t enc_pcm_size; /* PCM data length in bytes. Should be 240 bytes */
	ssize_t enc_frame_len; /* mSBC frame length, without H2 header. Should be 57 bytes */
	unsigned enc_frame_number;
	int enc_first_frame_sent;

	struct sine_generator gen;
	FILE *out_file;
	FILE *out_file2;
	int frame_counter;
};

int iothread_read_pcm_encode_msbc(struct ba_transport *t, struct sbc_state *sbc);
int iothread_read_msbc_decode_and_write_pcm(struct ba_transport *t, struct sbc_state *sbc);
struct sbc_state* iothread_initialize_msbc(struct sbc_state *sbc);
int iothread_write_encoded_data(int bt_fd, struct sbc_state *sbc, size_t length);

#endif
