/*
 * BlueALSA - io.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "io.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <sbc/sbc.h>
#if ENABLE_AAC
# include <fdk-aac/aacdecoder_lib.h>
# include <fdk-aac/aacenc_lib.h>
# define AACENCODER_LIB_VERSION LIB_VERSION( \
		AACENCODER_LIB_VL0, AACENCODER_LIB_VL1, AACENCODER_LIB_VL2)
#endif

#include "a2dp-codecs.h"
#include "hfp-codecs.h"
#include "a2dp-rtp.h"
#include "bluealsa.h"
#include "log.h"
#include "transport.h"
#include "utils.h"


struct io_sync {

	/* reference time point */
	struct timespec ts0;
	/* transfered frames since ts0 */
	uint32_t frames;
	/* used sampling frequency */
	uint16_t sampling;

};

/**
 * Wrapper for release callback, which can be used by pthread cleanup. */
static void io_thread_release(struct ba_transport *t) {

	/* During the normal operation mode, the release callback should not
	 * be NULL. Hence, we will relay on this callback - file descriptors
	 * are closed in it. */
	if (t->release != NULL)
		t->release(t);

	/* XXX: If the order of the cleanup push is right, this function will
	 *      indicate the end of the IO thread. */
	debug("Exiting IO thread");
}

/**
 * Open PCM for reading. */
static int io_thread_open_pcm_read(struct ba_pcm *pcm) {

	/* XXX: This check allows testing. During normal operation PCM FIFO
	 *      should not be opened outside the IO thread function. */
	if (pcm->fd == -1) {
		debug("Opening FIFO for reading: %s", pcm->fifo);
		/* this call will block until writing side is opened */
		if ((pcm->fd = open(pcm->fifo, O_RDONLY)) == -1)
			return -1;
	}

	return 0;
}

/**
 * Open PCM for writing. */
static int io_thread_open_pcm_write(struct ba_pcm *pcm) {

	/* transport PCM FIFO has not been requested */
	if (pcm->fifo == NULL) {
		errno = ENXIO;
		return -1;
	}

	if (pcm->fd == -1) {

		debug("Opening FIFO for writing: %s", pcm->fifo);
		int retries = 5;
		while(retries-- && pcm->fd == -1) {
			if ((pcm->fd = open(pcm->fifo, O_WRONLY | O_NONBLOCK)) == -1) {
				/* FIFO endpoint is not connected yet */
				debug("PCM write open failed: errno=%d", errno);
				//return -1;
			}
			usleep(10 * 1000);
		}
		if (pcm->fd == -1)
			return -1;

		/* Restore the blocking mode of our FIFO. Non-blocking mode was required
		 * only for the opening process - we do not want to block if the reading
		 * endpoint is not connected yet. On the other hand, blocking upon data
		 * write will prevent frame dropping. */
		fcntl(pcm->fd, F_SETFL, fcntl(pcm->fd, F_GETFL) & ~O_NONBLOCK);

		/* In order to receive EPIPE while writing to the pipe whose reading end
		 * is closed, the SIGPIPE signal has to be handled. For more information
		 * see the io_thread_write_pcm() function. */
		const struct sigaction sigact = { .sa_handler = SIG_IGN };
		sigaction(SIGPIPE, &sigact, NULL);

	}

	return 0;
}

/**
 * Scale PCM signal according to the transport audio properties. */
static void io_thread_scale_pcm(struct ba_transport *t, int16_t *buffer,
		size_t samples, int channels) {

	/* Get a snapshot of audio properties. Please note, that mutex is not
	 * required here, because we are not modifying these variables. */
	uint8_t ch1_volume = t->a2dp.ch1_volume;
	uint8_t ch2_volume = t->a2dp.ch2_volume;

	double ch1_scale = 0;
	double ch2_scale = 0;

	if (!t->a2dp.ch1_muted)
		ch1_scale = pow(10, (-64 + 64.0 * ch1_volume / 127 ) / 20);
	if (!t->a2dp.ch2_muted)
		ch2_scale = pow(10, (-64 + 64.0 * ch2_volume / 127 ) / 20);

	snd_pcm_scale_s16le(buffer, samples, channels, ch1_scale, ch2_scale);
}

/**
 * Read PCM signal from the transport PCM FIFO. */
static ssize_t io_thread_read_pcm(struct ba_pcm *pcm, int16_t *buffer, size_t size) {

	uint8_t *head = (uint8_t *)buffer;
	size_t len = size * sizeof(int16_t);
	ssize_t ret;

	/* This call will block until data arrives. If the passed file descriptor
	 * is invalid (e.g. -1) is means, that other thread (the controller) has
	 * closed the connection. If the connection was closed during the blocking
	 * part, we will still read correct data, because Linux kernel does not
	 * decrement file descriptor reference counter until the read returns. */
	while (len != 0 && (ret = read(pcm->fd, head, len)) != 0) {
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			break;
		}
		head += ret;
		len -= ret;
	}

	if (ret > 0)
		/* atomic data read is guaranteed */
		return size;

	if (ret == 0)
		debug("FIFO endpoint has been closed: %d", pcm->fd);
	if (errno == EBADF)
		ret = 0;
	if (ret == 0)
		transport_release_pcm(pcm);

	return ret;
}

/**
 * Write PCM signal to the transport PCM FIFO. */
static ssize_t io_thread_write_pcm(struct ba_pcm *pcm, const int16_t *buffer, size_t size) {

	const uint8_t *head = (uint8_t *)buffer;
	size_t len = size * sizeof(int16_t);
	ssize_t ret;

	do {
		if ((ret = write(pcm->fd, head, len)) == -1) {
			if (errno == EINTR)
				continue;
			if (errno == EPIPE) {
				/* This errno value will be received only, when the SIGPIPE
				 * signal is caught, blocked or ignored. */
				debug("FIFO endpoint has been closed: %d", pcm->fd);
				transport_release_pcm(pcm);
				return 0;
			}
			return ret;
		}
		head += ret;
		len -= ret;
	} while (len != 0);

	/* It is guaranteed, that this function will write data atomically. */
	return size;
}

/**
 * Convenient wrapper for writing to the RFCOMM socket. */
static ssize_t io_thread_write_rfcomm(int fd, const char *msg) {

	size_t len = strlen(msg);
	ssize_t ret;

retry:
	if ((ret = write(fd, msg, len)) == -1) {
		if (errno == EINTR)
			goto retry;
		error("RFCOMM write error: %s", strerror(errno));
	}

	return ret;
}

/**
 * Write AT command to the RFCOMM. */
static ssize_t io_thread_write_at_command(int fd, const char *msg) {

	char buffer[64];

	snprintf(buffer, sizeof(buffer), "%s\r", msg);
	return io_thread_write_rfcomm(fd, buffer);
}

/**
 * Write AT response code to the RFCOMM. */
static ssize_t io_thread_write_at_response(int fd, const char *msg) {

	char buffer[256];

	snprintf(buffer, sizeof(buffer), "\r\n%s\r\n", msg);
	return io_thread_write_rfcomm(fd, buffer);
}

/**
 * Synchronize thread timing with the audio sampling frequency.
 *
 * Time synchronization relies on the frame counter being linear. This counter
 * should be initialized upon transfer start and resume. With the size of this
 * counter being 32 bits, it is possible to track up to ~24 hours of playback,
 * with the sampling rate of 48 kHz. If this is insufficient, one can switch
 * to 64 bits, which would be sufficient to play for 12 million years. */
static int io_thread_time_sync(struct io_sync *io_sync, uint32_t frames) {

	const uint16_t sampling = io_sync->sampling;
	struct timespec ts_audio;
	struct timespec ts_clock;
	struct timespec ts;

	if (!frames)
		return 0;

	/* calculate the playback duration of given frames */
	unsigned int sec = frames / sampling;
	unsigned int res = frames % sampling;
	int duration = 1000000 * sec + 1000000 / sampling * res;

	io_sync->frames += frames;
	frames = io_sync->frames;

	/* keep transfer 10ms ahead */
	unsigned int overframes = sampling / 100;
	frames = frames > overframes ? frames - overframes : 0;

	ts_audio.tv_sec = frames / sampling;
	ts_audio.tv_nsec = 1000000000 / sampling * (frames % sampling);

	clock_gettime(CLOCK_MONOTONIC, &ts_clock);
	difftimespec(&io_sync->ts0, &ts_clock, &ts_clock);

	/* maintain constant bit rate */
	if (difftimespec(&ts_clock, &ts_audio, &ts) > 0)
		nanosleep(&ts, NULL);

	return duration;
}

void *io_thread_a2dp_sink_sbc(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	/* Cancellation should be possible only in the carefully selected place
	 * in order to prevent memory leaks and resources not being released. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(CANCEL_ROUTINE(io_thread_release), t);

	if (t->bt_fd == -1) {
		error("Invalid BT socket: %d", t->bt_fd);
		goto fail_init;
	}

	/* Check for invalid (e.g. not set) reading MTU. If buffer allocation does
	 * not return NULL (allocating zero bytes might return NULL), we will read
	 * zero bytes from the BT socket, which will be wrongly identified as a
	 * "connection closed" action. */
	if (t->mtu_read <= 0) {
		error("Invalid reading MTU: %zu", t->mtu_read);
		goto fail_init;
	}

	sbc_t sbc;

	if ((errno = -sbc_init_a2dp(&sbc, 0, t->a2dp.cconfig, t->a2dp.cconfig_size)) != 0) {
		error("Couldn't initialize SBC codec: %s", strerror(errno));
		goto fail_init;
	}

	const size_t sbc_codesize = sbc_get_codesize(&sbc);
	const size_t sbc_frame_len = sbc_get_frame_length(&sbc);

	const size_t in_buffer_size = t->mtu_read;
	const size_t out_buffer_size = sbc_codesize * (in_buffer_size / sbc_frame_len + 1);
	uint8_t *in_buffer = malloc(in_buffer_size);
	int16_t *out_buffer = malloc(out_buffer_size);

	pthread_cleanup_push(CANCEL_ROUTINE(sbc_finish), &sbc);
	pthread_cleanup_push(CANCEL_ROUTINE(free), in_buffer);
	pthread_cleanup_push(CANCEL_ROUTINE(free), out_buffer);

	if (in_buffer == NULL || out_buffer == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail;
	}

	struct pollfd pfds[] = {
		{ t->event_fd, POLLIN, 0 },
		{ -1, POLLIN, 0 },
	};

	debug("Starting IO loop: %s",
			bluetooth_profile_to_string(t->profile, t->codec));
	for (;;) {
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		ssize_t len;

		/* add BT socket to the poll if transport is active */
		pfds[1].fd = t->state == TRANSPORT_ACTIVE ? t->bt_fd : -1;

		if (poll(pfds, sizeof(pfds) / sizeof(*pfds), -1) == -1) {
			error("Transport poll error: %s", strerror(errno));
			goto fail;
		}

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */
			eventfd_t event;
			eventfd_read(pfds[0].fd, &event);
			continue;
		}

		if ((len = read(pfds[1].fd, in_buffer, in_buffer_size)) == -1) {
			debug("BT read error: %s", strerror(errno));
			continue;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		/* it seems that zero is never returned... */
		if (len == 0) {
			debug("BT socket has been closed: %d", pfds[1].fd);
			/* Prevent sending the release request to the BlueZ. If the socket has
			 * been closed, it means that BlueZ has already closed the connection. */
			close(pfds[1].fd);
			t->bt_fd = -1;
			goto fail;
		}

		if (io_thread_open_pcm_write(&t->a2dp.pcm) == -1) {
			if (errno != ENXIO)
				error("Couldn't open FIFO: %s", strerror(errno));
			continue;
		}

		const rtp_header_t *rtp_header = (rtp_header_t *)in_buffer;
		const rtp_payload_sbc_t *rtp_payload = (rtp_payload_sbc_t *)&rtp_header->csrc[rtp_header->cc];

		if (rtp_header->paytype != 96) {
			warn("Unsupported RTP payload type: %u", rtp_header->paytype);
			continue;
		}

		const uint8_t *input = (uint8_t *)(rtp_payload + 1);
		int16_t *output = out_buffer;
		size_t input_len = len - (input - in_buffer);
		size_t output_len = out_buffer_size;
		size_t frames = rtp_payload->frame_count;

		/* decode retrieved SBC frames */
		while (frames && input_len >= sbc_frame_len) {

			ssize_t len;
			size_t decoded;

			if ((len = sbc_decode(&sbc, input, input_len, output, output_len, &decoded)) < 0) {
				error("SBC decoding error: %s", strerror(-len));
				break;
			}

			input += len;
			input_len -= len;
			output += decoded / sizeof(int16_t);
			output_len -= decoded;
			frames--;

		}

		const size_t size = output - out_buffer;
		if (io_thread_write_pcm(&t->a2dp.pcm, out_buffer, size) == -1)
			error("FIFO write error: %s", strerror(errno));

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}

void *io_thread_a2dp_source_sbc(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(CANCEL_ROUTINE(io_thread_release), t);

	sbc_t sbc;

	if ((errno = -sbc_init_a2dp(&sbc, 0, t->a2dp.cconfig, t->a2dp.cconfig_size)) != 0) {
		error("Couldn't initialize SBC codec: %s", strerror(errno));
		goto fail_init;
	}

	const size_t sbc_codesize = sbc_get_codesize(&sbc);
	const size_t sbc_frame_len = sbc_get_frame_length(&sbc);
	const unsigned int channels = transport_get_channels(t);

	/* Writing MTU should be big enough to contain RTP header, SBC payload
	 * header and at least one SBC frame. In general, there is no constraint
	 * for the MTU value, but the speed might suffer significantly. */
	size_t mtu_write = t->mtu_write;
	if (mtu_write < sizeof(rtp_header_t) + sizeof(rtp_payload_sbc_t) + sbc_frame_len) {
		mtu_write = sizeof(rtp_header_t) + sizeof(rtp_payload_sbc_t) + sbc_frame_len;
		warn("Writing MTU too small for one single SBC frame: %zu < %zu", t->mtu_write, mtu_write);
	}

	const size_t in_buffer_size = sbc_codesize * (mtu_write / sbc_frame_len);
	const size_t out_buffer_size = mtu_write;
	int16_t *in_buffer = malloc(in_buffer_size);
	uint8_t *out_buffer = malloc(out_buffer_size);

	pthread_cleanup_push(CANCEL_ROUTINE(sbc_finish), &sbc);
	pthread_cleanup_push(CANCEL_ROUTINE(free), in_buffer);
	pthread_cleanup_push(CANCEL_ROUTINE(free), out_buffer);

	if (in_buffer == NULL || out_buffer == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail;
	}

	if (io_thread_open_pcm_read(&t->a2dp.pcm) == -1) {
		error("Couldn't open FIFO: %s", strerror(errno));
		goto fail;
	}

	uint16_t seq_number = random();
	uint32_t timestamp = random();

	/* initialize RTP header (the constant part) */
	rtp_header_t *rtp_header = (rtp_header_t *)out_buffer;
	memset(rtp_header, 0, sizeof(*rtp_header));
	rtp_header->version = 2;
	rtp_header->paytype = 96;

	rtp_payload_sbc_t *rtp_payload;
	rtp_payload = (rtp_payload_sbc_t *)&rtp_header->csrc[rtp_header->cc];
	memset(rtp_payload, 0, sizeof(*rtp_payload));

	/* reading head position and available read length */
	int16_t *in_buffer_head = in_buffer;
	size_t in_samples = in_buffer_size / sizeof(int16_t);

	struct pollfd pfds[] = {
		{ t->event_fd, POLLIN, 0 },
		{ -1, POLLIN, 0 },
	};

	struct io_sync io_sync = {
		.sampling = transport_get_sampling(t),
	};

	debug("Starting IO loop: %s",
			bluetooth_profile_to_string(t->profile, t->codec));
	for (;;) {
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		ssize_t samples;

		/* add PCM socket to the poll if transport is active */
		pfds[1].fd = t->state == TRANSPORT_ACTIVE ? t->a2dp.pcm.fd : -1;

		if (poll(pfds, sizeof(pfds) / sizeof(*pfds), -1) == -1) {
			error("Transport poll error: %s", strerror(errno));
			goto fail;
		}

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */
			eventfd_t event;
			eventfd_read(pfds[0].fd, &event);
			io_sync.frames = 0;
			continue;
		}

		/* read data from the FIFO - this function will block */
		if ((samples = io_thread_read_pcm(&t->a2dp.pcm, in_buffer_head, in_samples)) <= 0) {
			if (samples == -1)
				error("FIFO read error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		/* When the thread is created, there might be no data in the FIFO. In fact
		 * there might be no data for a long time - until client starts playback.
		 * In order to correctly calculate time drift, the zero time point has to
		 * be obtained after the stream has started. */
		if (io_sync.frames == 0)
			clock_gettime(CLOCK_MONOTONIC, &io_sync.ts0);

		if (!config.a2dp_volume)
			/* scale volume or mute audio signal */
			io_thread_scale_pcm(t, in_buffer_head, samples, channels);

		/* overall input buffer size */
		samples += in_buffer_head - in_buffer;

		const uint8_t *input = (uint8_t *)in_buffer;
		size_t input_len = samples * sizeof(int16_t);

		/* encode and transfer obtained data */
		while (input_len >= sbc_codesize) {

			uint8_t *output = (uint8_t *)(rtp_payload + 1);
			size_t output_len = out_buffer_size - (output - out_buffer);
			size_t pcm_frames = 0;
			size_t sbc_frames = 0;

			/* Generate as many SBC frames as possible to fill the output buffer
			 * without overflowing it. The size of the output buffer is based on
			 * the socket MTU, so such a transfer should be most efficient. */
			while (input_len >= sbc_codesize && output_len >= sbc_frame_len) {

				ssize_t len;
				ssize_t encoded;

				if ((len = sbc_encode(&sbc, input, input_len, output, output_len, &encoded)) < 0) {
					error("SBC encoding error: %s", strerror(-len));
					break;
				}

				input += len;
				input_len -= len;
				output += encoded;
				output_len -= encoded;
				pcm_frames += len / channels / sizeof(int16_t);
				sbc_frames++;

			}

			rtp_header->seq_number = htons(++seq_number);
			rtp_header->timestamp = htonl(timestamp);
			rtp_payload->frame_count = sbc_frames;

			if (write(t->bt_fd, out_buffer, output - out_buffer) == -1) {
				if (errno == ECONNRESET || errno == ENOTCONN) {
					/* exit the thread upon BT socket disconnection */
					debug("BT socket disconnected");
					goto fail;
				}
				error("BT socket write error: %s", strerror(errno));
			}

			/* keep data transfer at a constant bit rate, also
			 * get a timestamp for the next RTP frame */
			timestamp += io_thread_time_sync(&io_sync, pcm_frames);

		}

		/* convert bytes length to samples length */
		samples = input_len / sizeof(int16_t);

		/* If the input buffer was not consumed (due to codesize limit), we
		 * have to append new data to the existing one. Since we do not use
		 * ring buffer, we will simply move unprocessed data to the front
		 * of our linear buffer. */
		if (samples > 0 && (uint8_t *)in_buffer != input)
			memmove(in_buffer, input, samples * sizeof(int16_t));
		/* reposition our reading head */
		in_buffer_head = in_buffer + samples;
		in_samples = in_buffer_size / sizeof(int16_t) - samples;

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}

#if ENABLE_AAC
void *io_thread_a2dp_sink_aac(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(CANCEL_ROUTINE(io_thread_release), t);

	if (t->bt_fd == -1) {
		error("Invalid BT socket: %d", t->bt_fd);
		goto fail_open;
	}
	if (t->mtu_read <= 0) {
		error("Invalid reading MTU: %zu", t->mtu_read);
		goto fail_open;
	}

	HANDLE_AACDECODER handle;
	AAC_DECODER_ERROR err;

	if ((handle = aacDecoder_Open(TT_MP4_LATM_MCP1, 1)) == NULL) {
		error("Couldn't open AAC decoder");
		goto fail_open;
	}

	pthread_cleanup_push(CANCEL_ROUTINE(aacDecoder_Close), handle);

	const unsigned int channels = transport_get_channels(t);
#ifdef AACDECODER_LIB_VL0
	if ((err = aacDecoder_SetParam(handle, AAC_PCM_MIN_OUTPUT_CHANNELS, channels)) != AAC_DEC_OK) {
		error("Couldn't set min output channels: %s", aacdec_strerror(err));
		goto fail_init;
	}
	if ((err = aacDecoder_SetParam(handle, AAC_PCM_MAX_OUTPUT_CHANNELS, channels)) != AAC_DEC_OK) {
		error("Couldn't set max output channels: %s", aacdec_strerror(err));
		goto fail_init;
	}
#else
	if ((err = aacDecoder_SetParam(handle, AAC_PCM_OUTPUT_CHANNELS, channels)) != AAC_DEC_OK) {
		error("Couldn't set output channels: %s", aacdec_strerror(err));
		goto fail_init;
	}
#endif

	const size_t in_buffer_size = t->mtu_read;
	const size_t out_buffer_size = 2048 * channels * sizeof(INT_PCM);
	uint8_t *in_buffer = malloc(in_buffer_size);
	int16_t *out_buffer = malloc(out_buffer_size);

	pthread_cleanup_push(CANCEL_ROUTINE(free), in_buffer);
	pthread_cleanup_push(CANCEL_ROUTINE(free), out_buffer);

	if (in_buffer == NULL || out_buffer == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail;
	}

	struct pollfd pfds[] = {
		{ t->event_fd, POLLIN, 0 },
		{ -1, POLLIN, 0 },
	};

	debug("Starting IO loop: %s",
			bluetooth_profile_to_string(t->profile, t->codec));
	for (;;) {
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		CStreamInfo *aacinf;
		ssize_t len;

		/* add BT socket to the poll if transport is active */
		pfds[1].fd = t->state == TRANSPORT_ACTIVE ? t->bt_fd : -1;

		if (poll(pfds, sizeof(pfds) / sizeof(*pfds), -1) == -1) {
			error("Transport poll error: %s", strerror(errno));
			goto fail;
		}

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */
			eventfd_t event;
			eventfd_read(pfds[0].fd, &event);
			continue;
		}

		if ((len = read(pfds[1].fd, in_buffer, in_buffer_size)) == -1) {
			debug("BT read error: %s", strerror(errno));
			continue;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		/* it seems that zero is never returned... */
		if (len == 0) {
			debug("BT socket has been closed: %d", pfds[1].fd);
			/* Prevent sending the release request to the BlueZ. If the socket has
			 * been closed, it means that BlueZ has already closed the connection. */
			close(pfds[1].fd);
			t->bt_fd = -1;
			goto fail;
		}

		if (io_thread_open_pcm_write(&t->a2dp.pcm) == -1) {
			if (errno != ENXIO)
				error("Couldn't open FIFO: %s", strerror(errno));
			continue;
		}

		const rtp_header_t *rtp_header = (rtp_header_t *)in_buffer;
		uint8_t *rtp_latm = (uint8_t *)&rtp_header->csrc[rtp_header->cc];
		size_t rtp_latm_len = len - ((void *)rtp_latm - (void *)rtp_header);

		if (rtp_header->paytype != 96) {
			warn("Unsupported RTP payload type: %u", rtp_header->paytype);
			continue;
		}

		unsigned int data_len = rtp_latm_len;
		unsigned int valid = rtp_latm_len;

		if ((err = aacDecoder_Fill(handle, &rtp_latm, &data_len, &valid)) != AAC_DEC_OK)
			error("AAC buffer fill error: %s", aacdec_strerror(err));
		else if ((err = aacDecoder_DecodeFrame(handle, out_buffer, out_buffer_size, 0)) != AAC_DEC_OK)
			error("AAC decode frame error: %s", aacdec_strerror(err));
		else if ((aacinf = aacDecoder_GetStreamInfo(handle)) == NULL)
			error("Couldn't get AAC stream info");
		else {
			const size_t size = aacinf->frameSize * aacinf->numChannels;
			if (io_thread_write_pcm(&t->a2dp.pcm, out_buffer, size) == -1)
				error("FIFO write error: %s", strerror(errno));
		}

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
fail_open:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_AAC
void *io_thread_a2dp_source_aac(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;
	const a2dp_aac_t *cconfig = (a2dp_aac_t *)t->a2dp.cconfig;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(CANCEL_ROUTINE(io_thread_release), t);

	HANDLE_AACENCODER handle;
	AACENC_InfoStruct aacinf;
	AACENC_ERROR err;

	/* create AAC encoder without the Meta Data module */
	const unsigned int channels = transport_get_channels(t);
	if ((err = aacEncOpen(&handle, 0x07, channels)) != AACENC_OK) {
		error("Couldn't open AAC encoder: %s", aacenc_strerror(err));
		goto fail_open;
	}

	pthread_cleanup_push(CANCEL_ROUTINE(aacEncClose), &handle);

	unsigned int aot = AOT_NONE;
	unsigned int bitrate = AAC_GET_BITRATE(*cconfig);
	unsigned int samplerate = transport_get_sampling(t);
	unsigned int channelmode = channels == 1 ? MODE_1 : MODE_2;

	switch (cconfig->object_type) {
	case AAC_OBJECT_TYPE_MPEG2_AAC_LC:
#if AACENCODER_LIB_VERSION <= 0x03040C00 /* 3.4.12 */
		aot = AOT_MP2_AAC_LC;
		break;
#endif
	case AAC_OBJECT_TYPE_MPEG4_AAC_LC:
		aot = AOT_AAC_LC;
		break;
	case AAC_OBJECT_TYPE_MPEG4_AAC_LTP:
		aot = AOT_AAC_LTP;
		break;
	case AAC_OBJECT_TYPE_MPEG4_AAC_SCA:
		aot = AOT_AAC_SCAL;
		break;
	}

	if ((err = aacEncoder_SetParam(handle, AACENC_AOT, aot)) != AACENC_OK) {
		error("Couldn't set audio object type: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_BITRATE, bitrate)) != AACENC_OK) {
		error("Couldn't set bitrate: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_SAMPLERATE, samplerate)) != AACENC_OK) {
		error("Couldn't set sampling rate: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_CHANNELMODE, channelmode)) != AACENC_OK) {
		error("Couldn't set channel mode: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if (cconfig->vbr) {
		if ((err = aacEncoder_SetParam(handle, AACENC_BITRATEMODE, config.aac_vbr_mode)) != AACENC_OK) {
			error("Couldn't set VBR bitrate mode %u: %s", config.aac_vbr_mode, aacenc_strerror(err));
			goto fail_init;
		}
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_AFTERBURNER, config.aac_afterburner)) != AACENC_OK) {
		error("Couldn't enable afterburner: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_TRANSMUX, TT_MP4_LATM_MCP1)) != AACENC_OK) {
		error("Couldn't enable LATM transport type: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_HEADER_PERIOD, 1)) != AACENC_OK) {
		error("Couldn't set LATM header period: %s", aacenc_strerror(err));
		goto fail_init;
	}

	if ((err = aacEncEncode(handle, NULL, NULL, NULL, NULL)) != AACENC_OK) {
		error("Couldn't initialize AAC encoder: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncInfo(handle, &aacinf)) != AACENC_OK) {
		error("Couldn't get encoder info: %s", aacenc_strerror(err));
		goto fail_init;
	}

	int in_buffer_identifier = IN_AUDIO_DATA;
	int out_buffer_identifier = OUT_BITSTREAM_DATA;
	int in_buffer_element_size = sizeof(int16_t);
	int out_buffer_element_size = 1;
	int16_t *in_buffer, *in_buffer_head;
	uint8_t *out_buffer, *out_payload;
	int in_buffer_size;
	int out_payload_size;

	AACENC_BufDesc in_buf = {
		.numBufs = 1,
		.bufs = (void **)&in_buffer_head,
		.bufferIdentifiers = &in_buffer_identifier,
		.bufSizes = &in_buffer_size,
		.bufElSizes = &in_buffer_element_size,
	};
	AACENC_BufDesc out_buf = {
		.numBufs = 1,
		.bufs = (void **)&out_payload,
		.bufferIdentifiers = &out_buffer_identifier,
		.bufSizes = &out_payload_size,
		.bufElSizes = &out_buffer_element_size,
	};
	AACENC_InArgs in_args = { 0 };
	AACENC_OutArgs out_args = { 0 };

	in_buffer_size = in_buffer_element_size * aacinf.inputChannels * aacinf.frameLength;
	out_payload_size = aacinf.maxOutBufBytes;
	in_buffer = malloc(in_buffer_size);
	out_buffer = malloc(sizeof(rtp_header_t) + out_payload_size);

	pthread_cleanup_push(CANCEL_ROUTINE(free), in_buffer);
	pthread_cleanup_push(CANCEL_ROUTINE(free), out_buffer);

	if (in_buffer == NULL || out_buffer == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail;
	}

	uint16_t seq_number = random();
	uint32_t timestamp = random();

	/* initialize RTP header (the constant part) */
	rtp_header_t *rtp_header = (rtp_header_t *)out_buffer;
	memset(rtp_header, 0, sizeof(*rtp_header));
	rtp_header->version = 2;
	rtp_header->paytype = 96;

	/* anchor for RTP payload - audioMuxElement (RFC 3016) */
	out_payload = (uint8_t *)&rtp_header->csrc[rtp_header->cc];
	/* helper variable used during payload fragmentation */
	const size_t rtp_header_len = out_payload - out_buffer;

	if (io_thread_open_pcm_read(&t->a2dp.pcm) == -1) {
		error("Couldn't open FIFO: %s", strerror(errno));
		goto fail;
	}

	/* initial input buffer head position and the available size */
	size_t in_samples = in_buffer_size / in_buffer_element_size;
	in_buffer_head = in_buffer;

	struct pollfd pfds[] = {
		{ t->event_fd, POLLIN, 0 },
		{ -1, POLLIN, 0 },
	};

	struct io_sync io_sync = {
		.sampling = samplerate,
	};

	debug("Starting IO loop: %s",
			bluetooth_profile_to_string(t->profile, t->codec));
	for (;;) {
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		ssize_t samples;

		/* add PCM socket to the poll if transport is active */
		pfds[1].fd = t->state == TRANSPORT_ACTIVE ? t->a2dp.pcm.fd : -1;

		if (poll(pfds, sizeof(pfds) / sizeof(*pfds), -1) == -1) {
			error("Transport poll error: %s", strerror(errno));
			goto fail;
		}

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */
			eventfd_t event;
			eventfd_read(pfds[0].fd, &event);
			io_sync.frames = 0;
			continue;
		}

		/* read data from the FIFO - this function will block */
		if ((samples = io_thread_read_pcm(&t->a2dp.pcm, in_buffer_head, in_samples)) <= 0) {
			if (samples == -1)
				error("FIFO read error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		if (io_sync.frames == 0)
			clock_gettime(CLOCK_MONOTONIC, &io_sync.ts0);

		if (!config.a2dp_volume)
			/* scale volume or mute audio signal */
			io_thread_scale_pcm(t, in_buffer_head, samples, channels);

		/* overall input buffer size */
		samples += in_buffer_head - in_buffer;
		/* in the encoding loop head is used for reading */
		in_buffer_head = in_buffer;

		while ((in_args.numInSamples = samples) != 0) {

			if ((err = aacEncEncode(handle, &in_buf, &out_buf, &in_args, &out_args)) != AACENC_OK)
				error("AAC encoding error: %s", aacenc_strerror(err));

			if (out_args.numOutBytes > 0) {

				size_t payload_len_max = t->mtu_write - rtp_header_len;
				size_t payload_len = out_args.numOutBytes;
				rtp_header->timestamp = htonl(timestamp);

				/* If the size of the RTP packet exceeds writing MTU, the RTP payload
				 * should be fragmented. According to the RFC 3016, fragmentation of
				 * the audioMuxElement requires no extra header - the payload should
				 * be fragmented and spread across multiple RTP packets.
				 *
				 * TODO: Confirm that the fragmentation logic is correct.
				 *
				 * This code has been tested with Jabra Move headset, however the
				 * outcome of this test is not positive. Fragmented packets are not
				 * recognized by the device. */
				for (;;) {

					ssize_t ret;
					size_t len;

					len = payload_len > payload_len_max ? payload_len_max : payload_len;
					rtp_header->markbit = len < payload_len_max;
					rtp_header->seq_number = htons(++seq_number);

					if ((ret = write(t->bt_fd, out_buffer, rtp_header_len + len)) == -1) {
						if (errno == ECONNRESET || errno == ENOTCONN) {
							/* exit the thread upon BT socket disconnection */
							debug("BT socket disconnected");
							goto fail;
						}
						error("BT socket write error: %s", strerror(errno));
						break;
					}

					/* break if the last part of the payload has been written */
					if ((payload_len -= ret - rtp_header_len) == 0)
						break;

					/* move rest of data to the beginning of the payload */
					debug("Payload fragmentation: extra %zd bytes", payload_len);
					memmove(out_payload, out_payload + ret, payload_len);

				}

			}

			/* progress the head position by the number of samples consumed by the
			 * encoder, also adjust the number of samples in the input buffer */
			in_buffer_head += out_args.numInSamples;
			samples -= out_args.numInSamples;

			/* keep data transfer at a constant bit rate, also
			 * get a timestamp for the next RTP frame */
			timestamp += io_thread_time_sync(&io_sync, out_args.numInSamples / channels);

		}

		/* move leftovers to the beginning */
		if (samples > 0 && in_buffer != in_buffer_head)
			memmove(in_buffer, in_buffer_head, samples * in_buffer_element_size);
		/* reposition input buffer head */
		in_buffer_head = in_buffer + samples;
		in_samples = in_buffer_size / in_buffer_element_size - samples;

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
fail_open:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

enum at_cmd_type
{
	AT_CMD_TYPE_SET,
	AT_CMD_TYPE_GET,
	AT_CMD_TYPE_TEST,
};

#define AT_MAX_CMD_SIZE		16
#define AT_MAX_VALUE_SIZE	64

struct at_command
{
	enum at_cmd_type type;
	char command[AT_MAX_CMD_SIZE];
	char value[AT_MAX_VALUE_SIZE];
};

#if 0
void hexdump(const void *p, size_t len)
{
	unsigned char *ptr = (unsigned char*)p;
	while (len >= 16)
	{
		printf("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x"
				"        "
				"%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c\n",
				ptr[0], ptr[1], ptr[2], ptr[3],
				ptr[4], ptr[5], ptr[6], ptr[7],
				ptr[8], ptr[9], ptr[10], ptr[11],
				ptr[12], ptr[13], ptr[14], ptr[15],

				isprint(ptr[0]) ? ptr[0] : '.',
				isprint(ptr[1]) ? ptr[1] : '.',
				isprint(ptr[2]) ? ptr[2] : '.',
				isprint(ptr[3]) ? ptr[3] : '.',
				isprint(ptr[4]) ? ptr[4] : '.',
				isprint(ptr[5]) ? ptr[5] : '.',
				isprint(ptr[6]) ? ptr[6] : '.',
				isprint(ptr[7]) ? ptr[7] : '.',
				isprint(ptr[8]) ? ptr[8] : '.',
				isprint(ptr[9]) ? ptr[9] : '.',
				isprint(ptr[10]) ? ptr[10] : '.',
				isprint(ptr[11]) ? ptr[11] : '.',
				isprint(ptr[12]) ? ptr[12] : '.',
				isprint(ptr[13]) ? ptr[13] : '.',
				isprint(ptr[14]) ? ptr[14] : '.',
				isprint(ptr[15]) ? ptr[15] : '.');


		len -= 16;
		ptr += 16;
	}

	if (len)
	{
		for (int i = 0; i < len; i++)
			printf("%02x ", ptr[i]);

		for (int i = 0; i < ((16 - len) * 3 - 1 + 8); i++)
			printf(" ");

		for (int i = 0; i < len; i++)
			printf("%c", isprint(ptr[i]) ? ptr[i] : '.');

		printf("\n");
	}
}
#endif

/* str gets modified */
int at_parse(char *str, struct at_command *cmd)
{
	memset(cmd->command, 0, sizeof(cmd->command));
	memset(cmd->value, 0, sizeof(cmd->value));

	/* Skip initial whitespace */
	const char *s = str;
	while (isspace(*s))
		s++;

	/* Remove trailing whitespace */
	char *end = str + strlen(str) - 1;
	while(end >= s && isspace(*end))
		*end-- = '\0';

	/* starts with AT? */
	if (strncasecmp(s, "AT", 2))
		return -1;

	/* Can we find equals sign? */
	char *equal = strstr(s, "=");
	if (equal != NULL)
	{
		/* Set (ATxxx=value or test (ATxxx=?) */
		strncpy(cmd->command, s + 2, equal - s - 2);

		if (equal[1] == '?')
		{
			cmd->type = AT_CMD_TYPE_TEST;
		}
		else
		{
			cmd->type = AT_CMD_TYPE_SET;
			strncpy(cmd->value, equal + 1, AT_MAX_VALUE_SIZE - 1);
		}
	}
	else
	{
		/* Get (ATxxx?) */
		cmd->type = AT_CMD_TYPE_GET;
		char *question = strstr(s, "?");
		if (question != NULL )
			strncpy(cmd->command, s + 2, question - s - 2);
		else
			return -1;
	}
	debug("Got %s\ntype = %d\ncommand = %s\nvalue = %s", str, cmd->type, cmd->command, cmd->value);
	return 0;
}

#define HFP_AG_FEAT_CODEC	(1 << 9)
#define HFP_HF_FEAT_CODEC	(1 << 7)
#define HFP_AG_FEAT_ECS		(1 << 6)

#define HFP_AG_FEATURES		 HFP_AG_FEAT_ECS

void *io_thread_rfcomm(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(CANCEL_ROUTINE(io_thread_release), t);

	uint8_t mic_gain = t->rfcomm.sco->sco.mic_gain;
	uint8_t spk_gain = t->rfcomm.sco->sco.spk_gain;
	char buffer[64];

	struct pollfd pfds[] = {
		{ t->event_fd, POLLIN, 0 },
		{ t->bt_fd, POLLIN, 0 },
	};

	/* Default to CVSD codec */
	t->rfcomm.sco->sco.codec = SCO_CODEC_CVSD;

	debug("Starting RFCOMM loop: %s",
			bluetooth_profile_to_string(t->profile, t->codec));
	for (;;) {

		const char *response = "OK";
		struct at_command at;
		ssize_t ret;

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		ret = poll(pfds, sizeof(pfds) / sizeof(*pfds), -1);
		debug("poll ret=%zd", ret);
		//if (poll(pfds, sizeof(pfds) / sizeof(*pfds), -1) == -1) {
		if (ret == -1) {
			error("Transport poll error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */

			eventfd_t event;
			eventfd_read(pfds[0].fd, &event);

			if (mic_gain != t->rfcomm.sco->sco.mic_gain) {
				mic_gain = t->rfcomm.sco->sco.mic_gain;
				debug("Setting microphone gain: %d", mic_gain);
				sprintf(buffer, "+VGM=%d", mic_gain);
				io_thread_write_at_response(pfds[1].fd, buffer);
			}
			if (spk_gain != t->rfcomm.sco->sco.spk_gain) {
				spk_gain = t->rfcomm.sco->sco.spk_gain;
				debug("Setting speaker gain: %d", spk_gain);
				sprintf(buffer, "+VGS=%d", mic_gain);
				io_thread_write_at_response(pfds[1].fd, buffer);
			}

			continue;
		}

		memset(buffer, 0, sizeof(buffer));
		if ((ret = read(pfds[1].fd, buffer, sizeof(buffer))) == -1) {
			switch (errno) {
			case ECONNABORTED:
			case ECONNRESET:
			case ENOTCONN:
			case ETIMEDOUT:
				/* exit the thread upon socket disconnection */
				debug("RFCOMM disconnected: %s", strerror(errno));
				transport_set_state(t, TRANSPORT_ABORTED);
				goto fail;
			default:
				error("RFCOMM read error: %s", strerror(errno));
				continue;
			}
		}

		/* Parse AT command received from the headset. */
		debug("AT: %s\n", buffer);

		if (at_parse(buffer, &at)) {
			warn("Invalid AT command: %s", buffer);
			continue;
		}

		if (strcmp(at.command, "RING") == 0) {
		}
		else if (strcmp(at.command, "+CKPD") == 0 && atoi(at.value) == 200) {
		}
		else if (strcmp(at.command, "+VGM") == 0)
			t->rfcomm.sco->sco.mic_gain = mic_gain = atoi(at.value);
		else if (strcmp(at.command, "+VGS") == 0)
			t->rfcomm.sco->sco.spk_gain = spk_gain = atoi(at.value);
		else if (strcmp(at.command, "+IPHONEACCEV") == 0) {

			char *ptr = at.value;
			size_t count = atoi(strsep(&ptr, ","));
			char tmp;

			while (count-- && ptr != NULL)
				switch (tmp = *strsep(&ptr, ",")) {
				case '1':
					if (ptr != NULL)
						t->device->xapl.accev_battery = atoi(strsep(&ptr, ","));
					break;
				case '2':
					if (ptr != NULL)
						t->device->xapl.accev_docked = atoi(strsep(&ptr, ","));
					break;
				default:
					warn("Unsupported IPHONEACCEV key: %c", tmp);
					strsep(&ptr, ",");
				}

		}
		else if (strcmp(at.command, "+XAPL") == 0) {

			unsigned int vendor, product;
			unsigned int version, features;

			if (sscanf(at.value, "%x-%x-%u,%u", &vendor, &product, &version, &features) == 4) {
				t->device->xapl.vendor_id = vendor;
				t->device->xapl.product_id = product;
				t->device->xapl.version = version;
				t->device->xapl.features = features;
				response = "+XAPL=BlueALSA,0";
			}
			else {
				warn("Invalid XAPL value: %s", at.value);
				response = "ERROR";
			}

		}
		else if (strcmp(at.command, "+BRSF") == 0) {
			uint32_t hf_features = strtoul(at.value, NULL, 10);
			debug("Got HF features: 0x%x", hf_features);

			uint32_t ag_features = HFP_AG_FEATURES;
#if defined(ENABLE_MSBC)
			if (hf_features & (HFP_HF_FEAT_CODEC)) {
				ag_features |= HFP_AG_FEAT_CODEC;
			}
			else
#endif
		       	{
				/* Codec negotiation is not supported,
				hence no wideband audio support.
				AT+BAC is not sent*/
				t->rfcomm.sco->sco.codec = SCO_CODEC_CVSD;
			}

			t->rfcomm.sco->sco.hf_features = hf_features;

			snprintf(buffer, sizeof(buffer), "+BRSF: %u", ag_features);
			io_thread_write_at_response(pfds[1].fd, buffer);
		}
		else if (strcmp(at.command, "+BAC") == 0 && at.type == AT_CMD_TYPE_SET) {
			debug("Supported codecs: %s", at.value);
			/* Split codecs string */
			gchar **codecs = g_strsplit(at.value, ",", 0);
			for (int i = 0; codecs[i]; i++) {
				gchar *codec = codecs[i];
				uint32_t codec_value = strtoul(codec, NULL, 10);
				if (codec_value == SCO_CODEC_MSBC) {
					t->rfcomm.sco->sco.codec = SCO_CODEC_MSBC;
				}
			}
			g_strfreev(codecs);
		}
		else if (strcmp(at.command, "+CIND") == 0) {
			if ( at.type == AT_CMD_TYPE_GET) {
				io_thread_write_at_response(pfds[1].fd,
					"+CIND: 0,0,1,4,0,4,0");
			}
			else if(at.type == AT_CMD_TYPE_TEST) {
				io_thread_write_at_response(pfds[1].fd,
					"+CIND: "
					"(\"call\",(0,1))"
					",(\"callsetup\",(0-3))"
					",(\"service\",(0-1))"
					",(\"signal\",(0-5))"
					",(\"roam\",(0,1))"
					",(\"battchg\",(0-5))"
					",(\"callheld\",(0-2))"
					);
			}
		}
		else if (strcmp(at.command, "+CMER") == 0 && at.type == AT_CMD_TYPE_SET) {
			/* +CMER is the last step of the "Service Level
			   Connection establishment" procedure */

			/* Send OK */
			io_thread_write_at_response(pfds[1].fd, response);
			/* Send codec select */
			if (t->rfcomm.sco->sco.codec != SCO_CODEC_CVSD) {
				snprintf(buffer, sizeof(buffer), "+BCS: %u", t->rfcomm.sco->sco.codec);
				io_thread_write_at_response(pfds[1].fd, buffer);
			}
			continue;
		}
		else if (strcmp(at.command, "+BCS") == 0 && at.type == AT_CMD_TYPE_SET) {
			debug("Got codec selected: %d", atoi(at.value));
		}
		else if (strcmp(at.command, "+BTRH") == 0 && at.type == AT_CMD_TYPE_GET) {
		}
		else if (strcmp(at.command, "+NREC") == 0 && at.type == AT_CMD_TYPE_SET) {
		}
		else if (strcmp(at.command, "+CCWA") == 0 && at.type == AT_CMD_TYPE_SET) {
		}
		else if (strcmp(at.command, "+BIA") == 0 && at.type == AT_CMD_TYPE_SET) {
		}
		else if (strcmp(at.command, "+CHLD") == 0 && at.type == AT_CMD_TYPE_TEST) {
			io_thread_write_at_response(pfds[1].fd, "+CHLD: (0,1,2,3)");
		}
		else {
			warn("Unsupported AT command: %s", buffer);
			response = "ERROR";
		}

		io_thread_write_at_response(pfds[1].fd, response);
	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	return NULL;
}

#define SCO_H2_HDR_LEN		2
#define MSBC_FRAME_LEN		57
#define SCO_H2_FRAME_LEN	(SCO_H2_HDR_LEN + MSBC_FRAME_LEN)

#define MSBC_PCM_LEN		240
#define SCO_H2_HDR_0		0x01
#define MSBC_SYNC		0xAD

static int io_thread_read_pcm_write_bt(struct ba_pcm *pcm, int16_t *buffer, ssize_t samples, int bt_fd)
{
	/* read data from the FIFO - this function will block */
	if ((samples = samples = io_thread_read_pcm(pcm, buffer, samples)) <= 0) {
		if (samples == -1) {
			error("FIFO read error: %s", strerror(errno));
			return -1;
		}
	}

	int err = write(bt_fd, buffer, samples * sizeof(int16_t));
	if (err == -1) {
		error("SCO socket write error: %s", strerror(errno));
		return -1;
	}

	return samples;
}

#if defined(ENABLE_MSBC)
struct msbc_frame {
	uint8_t h2_header[SCO_H2_HDR_LEN];
	uint8_t payload[MSBC_FRAME_LEN];
	uint8_t padding;
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
	uint8_t enc_buffer[SCO_H2_FRAME_LEN * 4];

	size_t enc_pcm_buffer_cnt; /* e.g. bytes of data in buffer */
	size_t enc_pcm_buffer_size; /* in bytes */
	uint8_t enc_pcm_buffer[MSBC_PCM_LEN * 4];
	ssize_t enc_pcm_size; /* PCM data length in bytes. Should be 240 bytes */
	ssize_t enc_frame_len; /* mSBC frame length, without H2 header. Should be 57 bytes */
	unsigned enc_frame_number;
};

#if defined (SILENCE)
uint8_t msbc_zero[] = {
	0xad, 0x0, 0x0, 0xc5, 0x0, 0x0, 0x0, 0x0, 0x77, 0x6d, 0xb6, 0xdd,
	0xdb, 0x6d, 0xb7, 0x76, 0xdb, 0x6d, 0xdd, 0xb6, 0xdb, 0x77, 0x6d,
	0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76, 0xdb, 0x6d, 0xdd, 0xb6, 0xdb,
	0x77, 0x6d, 0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76, 0xdb, 0x6d, 0xdd,
	0xb6, 0xdb, 0x77, 0x6d, 0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76, 0xdb,
	0x6c
};
#endif

int iothread_write_encoded_data(int bt_fd, struct sbc_state *sbc, size_t length)
{
	size_t written = 0;

	if (sbc->enc_buffer_cnt < length) {
		warn("Encoded data underflow");
		return -1;
	}

	if ((written = write(bt_fd, sbc->enc_buffer, length)) == -1) {
		if (errno != EWOULDBLOCK && errno != EAGAIN)
			warn("Could not write to mSBC socket: %s", strerror(errno));
		return -1;
	}

	memmove(sbc->enc_buffer,
		sbc->enc_buffer + written,
		sbc->enc_buffer_cnt - written);

	sbc->enc_buffer_cnt -= written;

	return 0;
}

static struct sbc_state* iothread_initialize_msbc(struct sbc_state *sbc)
{
	if (!sbc) {
		sbc = malloc(sizeof(*sbc));
		if (!sbc) {
			error("Cannot allocate SBC");
			return NULL;
		}
	}

	memset(sbc, 0, sizeof(*sbc));

	if (errno = -sbc_init_msbc(&sbc->dec, 0) != 0) {
		error("Couldn't initialize mSBC decoder: %s", strerror(errno));
		goto fail;
	}

	if (errno = -sbc_init_msbc(&sbc->enc, 0) != 0) {
		error("Couldn't initialize mSBC decoder: %s", strerror(errno));
		goto fail;
	}

	sbc->sbc_frame_len = sbc_get_frame_length(&sbc->dec);
	sbc->dec_buffer_size = sizeof(sbc->dec_buffer);

	sbc->enc_pcm_size = sbc_get_codesize(&sbc->enc);
	sbc->enc_frame_len = sbc_get_frame_length(&sbc->enc);
	sbc->enc_buffer_size = sizeof(sbc->enc_buffer);
	sbc->enc_pcm_buffer_size = sizeof(sbc->enc_pcm_buffer);
	if (sbc->enc_frame_len != MSBC_FRAME_LEN) {
		error("Unexpected mSBC frame size: %zd", sbc->enc_frame_len); 
	}

	return sbc;

fail:
	free(sbc);

	return NULL;
}

void iothread_encode_msbc_frames(struct sbc_state *sbc)
{
	static const uint8_t h2_header_frame_number[] = {
		0x08, 0x38, 0xc8, 0xf8
	};

	size_t written, pcm_consumed = 0;
	ssize_t len;

	/* Encode all we can */
	while ((sbc->enc_pcm_buffer_cnt - pcm_consumed) >= sbc->enc_pcm_size &&
	       (sbc->enc_buffer_size - sbc->enc_buffer_cnt) >= SCO_H2_FRAME_LEN) {

		struct msbc_frame *frame = (struct msbc_frame*)(sbc->enc_buffer
								+ sbc->enc_buffer_cnt);

		if ((len = sbc_encode(&sbc->enc,
				      sbc->enc_pcm_buffer + pcm_consumed,
				      sbc->enc_pcm_buffer_cnt - pcm_consumed,
				      frame->payload,
				      sizeof(frame->payload),
				      &written)) < 0) {
			error("Unable to encode mSBC: %s", strerror(-len));
			return;
		};

		pcm_consumed += len;

		frame->h2_header[0] = SCO_H2_HDR_0;
		frame->h2_header[1] = h2_header_frame_number[sbc->enc_frame_number];
		sbc->enc_frame_number = ((sbc->enc_frame_number) + 1) % 4;
		sbc->enc_buffer_cnt += sizeof(*frame);

#ifdef SILENCE
		memcpy(frame->payload, msbc_zero, sizeof(frame->payload));
#endif
	}
	/* Reshuffle remaining PCM samples to the beginning of the buffer */
	memmove(sbc->enc_pcm_buffer,
		sbc->enc_pcm_buffer + pcm_consumed,
		sbc->enc_pcm_buffer_cnt - pcm_consumed);

	/* And deduct consumed data */
	sbc->enc_pcm_buffer_cnt -= pcm_consumed;
}

void iothread_find_and_decode_msbc(int pcm_fd, struct sbc_state *sbc)
{
	ssize_t len;
	size_t bytes_left = sbc->dec_buffer_cnt;
	uint8_t *p = (uint8_t*) sbc->dec_buffer;

	/* Find frame start */
	while (bytes_left >= SCO_H2_HDR_LEN + sbc->sbc_frame_len) {
		if (p[0] == SCO_H2_HDR_0 && p[2] == MSBC_SYNC) {
			/* Found frame.  TODO: Check SEQ, implement PLC */
			size_t decoded = 0;
			if ((len = sbc_decode(&sbc->dec,
					      p + 2,
					      sbc->sbc_frame_len,
					      sbc->dec_pcm_buffer,
					      sizeof(sbc->dec_pcm_buffer),
					      &decoded)) < 0) {
				error("mSBC decoding error: %s\n", strerror(-len));
				sbc->dec_buffer_cnt = 0;
				return;
			}
			bytes_left -= len + SCO_H2_HDR_LEN;
			p += len + SCO_H2_HDR_LEN;
			write(pcm_fd, sbc->dec_pcm_buffer, decoded);
		}
		else {
			bytes_left--;
			p++;
		}
	}
	memmove(sbc->dec_buffer, p, bytes_left);
	sbc->dec_buffer_cnt = bytes_left;
}
#endif

void *io_thread_sco(void *arg) {

	struct ba_transport *t = (struct ba_transport *)arg;

	/* this buffer has to be bigger than SCO MTU */
	const size_t pcm_buffer_size = 512;
	int16_t *pcm_buffer = malloc(pcm_buffer_size);

	int using_msbc = 0;
	struct sbc_state *sbc = NULL;

	pthread_cleanup_push(CANCEL_ROUTINE(free), pcm_buffer);
	pthread_cleanup_push(CANCEL_ROUTINE(free), sbc);

	if (pcm_buffer == NULL) {
		error("Couldn't create data buffers: %s", strerror(ENOMEM));
		goto fail;
	}

	struct pollfd pfds[] = {
		{ t->event_fd, POLLIN, 0 },
		{ -1, POLLIN, 0 },
		{ -1, POLLIN, 0 },
	};

	struct io_sync io_sync = {
		.sampling = transport_get_sampling(t),
	};

	debug("Starting IO loop: %s",
			bluetooth_profile_to_string(t->profile, t->codec));
	for (;;) {

		pfds[1].fd = t->bt_fd;
		pfds[2].fd = t->sco.spk_pcm.fd;

		if (poll(pfds, sizeof(pfds) / sizeof(*pfds), -1) == -1) {
			error("Transport poll error: %s", strerror(errno));
			goto fail;
		}

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */

			eventfd_t event;
			eventfd_read(pfds[0].fd, &event);

			/* Try to open reading and/or writing PCM file descriptor. Note,
			 * that we are not checking for errors, because we don't care. */
			io_thread_open_pcm_read(&t->sco.spk_pcm);
			io_thread_open_pcm_write(&t->sco.mic_pcm);

			/* It is required to release SCO if we are not transferring audio,
			 * because it will free Bluetooth bandwidth - microphone signal is
			 * transfered even though we are not reading from it! */
			if (t->sco.spk_pcm.fd == -1 && t->sco.mic_pcm.fd == -1) {
				transport_release_bt_sco(t);
				io_sync.frames = 0;
			}
			else {
				transport_acquire_bt_sco(t);

				fcntl(t->bt_fd, F_SETFL, fcntl(t->bt_fd, F_GETFL) | O_NONBLOCK);

#if defined(ENABLE_MSBC)
				/* This can be called again, make sure it is "reentrant" */
				if (t->sco.codec == SCO_CODEC_MSBC) {
					sbc = iothread_initialize_msbc(sbc);
					if (!sbc)
						goto fail;
					using_msbc = 1;
				}
#endif
				io_sync.sampling = transport_get_sampling(t);
			}

			continue;
		}

		if (io_sync.frames == 0)
			clock_gettime(CLOCK_MONOTONIC, &io_sync.ts0);

		/* Bluetooth socket */
		if (pfds[1].revents & POLLIN) {

			ssize_t len;

#if defined(ENABLE_MSBC)
			if (t->sco.codec == SCO_CODEC_MSBC) {

				uint8_t *read_buf = sbc->dec_buffer + sbc->dec_buffer_cnt;
				size_t read_buf_size = sbc->dec_buffer_size - sbc->dec_buffer_cnt;

				if ((len = read(pfds[1].fd, read_buf, read_buf_size)) == -1) {
					debug("SCO read error: %s", strerror(errno));
					continue;
				}

				sbc->dec_buffer_cnt += len;

				if (t->sco.mic_pcm.fd >= 0)
					iothread_find_and_decode_msbc(t->sco.mic_pcm.fd, sbc);
				else
					sbc->dec_buffer_cnt = 0; /* Drop microphone data if PCM isn't open */

				/* Synchronize write to read */
				if (t->sco.spk_pcm.fd >= 0) {
					iothread_write_encoded_data(pfds[1].fd, sbc, 24);
					if ((sbc->enc_buffer_size - sbc->enc_buffer_cnt) >= SCO_H2_FRAME_LEN) {
						pfds[2].events = POLLIN;
					}
				}
			}
			else
#endif
		       	{
				/* Read CVSD data from socket and write to PCM FIFO */
				if ((len = read(pfds[1].fd, pcm_buffer, pcm_buffer_size)) == -1) {
					debug("SCO read error: %s", strerror(errno));
					continue;
				}

				/* "detect" MTU on the fly */
				if (t->mtu_write == 0) {
					t->mtu_write = len;
					t->mtu_read = len;
				}

				if (t->sco.mic_pcm.fd >= 0)
					write(t->sco.mic_pcm.fd, pcm_buffer, len);
			}
		}

                /* PCM in FIFO */
		if (pfds[2].revents & POLLIN) {

#if defined(ENABLE_MSBC)
			if (t->sco.codec == SCO_CODEC_MSBC) {

				ssize_t len;

				/* Read PCM samples */
				if ((len = read(t->sco.spk_pcm.fd,
						sbc->enc_pcm_buffer + sbc->enc_pcm_buffer_cnt,
						sbc->enc_pcm_buffer_size - sbc->enc_pcm_buffer_cnt)) == -1) {
					error("Unable to read PCM data: %s", strerror(errno));
					continue;
				}
				sbc->enc_pcm_buffer_cnt += len;

				iothread_encode_msbc_frames(sbc);

				/* Stop reading until there is enough space for another frame */
				pfds[2].events = 0;

			}
			else
#endif
		       	{
				io_thread_read_pcm_write_bt(&t->sco.spk_pcm, (int16_t*) pcm_buffer,
						t->mtu_write / sizeof(int16_t), t->bt_fd);

				/* keep data transfer at a constant bit rate */
				io_thread_time_sync(&io_sync, t->mtu_write / sizeof(int16_t));
			}
		}


	}

fail:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

