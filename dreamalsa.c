/*
 * Dream One/Two do not route DVB audio memory-source writes to the physical
 * output.  Reuse the ALSA backend shipped by gst-plugin-dreamaudiosink at
 * runtime, without adding a link-time GStreamer/FFmpeg dependency to the
 * generic libdreamdvd package.
 */

#include "dreamalsa.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DREAM_AUDIO_SINK "/usr/lib/gstreamer-1.0/libgstdreamaudiosink.so"
#define DREAM_ALSA_RETRY_US 2000
#define DREAM_COMPRESSED_BUFFER_SIZE (128 * 1024)
#define DREAM_AUDIO_QUEUE_SIZE (512 * 1024)
/* enum AVCodecID keeps its first compressed-audio range stable.  The Dream
 * decoder API deliberately accepts int so its public header stays FFmpeg-free. */
#define DDVD_AV_CODEC_ID_AC3 0x15003
#define DDVD_AV_CODEC_ID_DTS 0x15004

enum ddvd_dream_decoder_output_type {
	DDVD_DREAM_DECODER_OUTPUT_PCM,
	DDVD_DREAM_DECODER_OUTPUT_IEC61937
};

typedef void (*dream_decoder_output_cb_t)(
	enum ddvd_dream_decoder_output_type type,
	unsigned int sample_rate,
	unsigned int channels,
	const uint8_t *data,
	size_t size,
	int64_t pts,
	void *user_data);

typedef void *(*dream_alsa_new_t)(const char *device);
typedef void (*dream_alsa_free_t)(void *alsa);
typedef int (*dream_alsa_set_params_t)(void *alsa, unsigned int sample_rate,
	unsigned int channels, unsigned int bytes_per_sample, int passthrough);
typedef void (*dream_alsa_set_nonblocking_t)(void *alsa, int enabled);
typedef int (*dream_alsa_write_t)(void *alsa, const uint8_t *data, size_t size,
	int64_t pts);
typedef void (*dream_alsa_drop_t)(void *alsa);
typedef void (*dream_alsa_reset_anchor_t)(void *alsa);
typedef void *(*dream_decoder_new_t)(int codec_id, int sample_rate,
	int channels, const void *extradata, int extradata_size,
	dream_decoder_output_cb_t callback, void *user_data);
typedef void (*dream_decoder_free_t)(void *decoder);
typedef int (*dream_decoder_decode_t)(void *decoder, const uint8_t *data,
	int size, int64_t pts, int64_t dts);

static void *dream_audio_handle;
static void *dream_alsa;
static void *dream_decoder;
static int dream_alsa_configured;
static unsigned int dream_alsa_rate;
static unsigned int dream_alsa_channels;
static int dream_alsa_passthrough;
static dream_alsa_new_t dream_alsa_new;
static dream_alsa_free_t dream_alsa_free;
static dream_alsa_set_params_t dream_alsa_set_params;
static dream_alsa_set_nonblocking_t dream_alsa_set_nonblocking;
static dream_alsa_write_t dream_alsa_write;
static dream_alsa_drop_t dream_alsa_drop;
static dream_alsa_reset_anchor_t dream_alsa_reset_anchor;
static dream_decoder_new_t dream_decoder_new;
static dream_decoder_free_t dream_decoder_free;
static dream_decoder_decode_t dream_decoder_decode;
static int dream_decoder_codec_id = -1;
static uint8_t dream_ac3_buffer[DREAM_COMPRESSED_BUFFER_SIZE];
static size_t dream_ac3_buffer_size;
static uint8_t dream_dts_buffer[DREAM_COMPRESSED_BUFFER_SIZE];
static size_t dream_dts_buffer_size;

struct ddvd_dream_audio_packet {
	uint8_t *data;
	size_t size;
	unsigned int sample_rate;
	unsigned int channels;
	int passthrough;
	struct ddvd_dream_audio_packet *next;
};

static pthread_t dream_audio_queue_thread;
static pthread_mutex_t dream_audio_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dream_audio_queue_cond = PTHREAD_COND_INITIALIZER;
static struct ddvd_dream_audio_packet *dream_audio_queue_head;
static struct ddvd_dream_audio_packet *dream_audio_queue_tail;
static size_t dream_audio_queue_bytes;
static int dream_audio_queue_running;
static int dream_audio_queue_stop_requested;
static int dream_audio_queue_paused;
static int dream_audio_queue_failed;

static int ddvd_dream_audio_queue_stopping(void)
{
	int stopping;

	pthread_mutex_lock(&dream_audio_queue_mutex);
	stopping = dream_audio_queue_stop_requested;
	pthread_mutex_unlock(&dream_audio_queue_mutex);
	return stopping;
}

static int ddvd_dream_alsa_write_all(const uint8_t *data, size_t size)
{
	size_t written = 0;

	while (written < size) {
		int result;

		if (ddvd_dream_audio_queue_stopping())
			break;
		result = dream_alsa_write(dream_alsa, data + written,
			size - written, -1);

		if (result < 0)
			return written ? (int)written : -1;
		if (result == 0) {
			usleep(DREAM_ALSA_RETRY_US);
			continue;
		}
		if ((size_t)result > size - written)
			return written ? (int)written : -1;
		written += (size_t)result;
	}
	return (int)written;
}

static void ddvd_dream_audio_queue_clear_locked(void)
{
	while (dream_audio_queue_head != NULL) {
		struct ddvd_dream_audio_packet *packet = dream_audio_queue_head;

		dream_audio_queue_head = packet->next;
		free(packet->data);
		free(packet);
	}
	dream_audio_queue_tail = NULL;
	dream_audio_queue_bytes = 0;
}

static int ddvd_dream_alsa_configure(unsigned int sample_rate,
	unsigned int channels, int passthrough)
{
	if (dream_alsa_configured && dream_alsa_rate == sample_rate &&
		dream_alsa_channels == channels &&
		dream_alsa_passthrough == passthrough)
		return 0;

	if (dream_alsa_configured)
		dream_alsa_drop(dream_alsa);
	if (dream_alsa_set_params(dream_alsa, sample_rate, channels, 2,
		passthrough) < 0)
		return -1;
	dream_alsa_configured = 1;
	dream_alsa_rate = sample_rate;
	dream_alsa_channels = channels;
	dream_alsa_passthrough = passthrough;
	return 0;
}

static void *ddvd_dream_audio_queue_worker(void *unused)
{
	(void)unused;
	for (;;) {
		struct ddvd_dream_audio_packet *packet;
		int result;

		pthread_mutex_lock(&dream_audio_queue_mutex);
		while (!dream_audio_queue_stop_requested &&
			(dream_audio_queue_paused || dream_audio_queue_head == NULL))
			pthread_cond_wait(&dream_audio_queue_cond,
				&dream_audio_queue_mutex);
		if (dream_audio_queue_stop_requested) {
			pthread_mutex_unlock(&dream_audio_queue_mutex);
			break;
		}

		packet = dream_audio_queue_head;
		dream_audio_queue_head = packet->next;
		if (dream_audio_queue_head == NULL)
			dream_audio_queue_tail = NULL;
		dream_audio_queue_bytes -= packet->size;
		pthread_cond_broadcast(&dream_audio_queue_cond);
		pthread_mutex_unlock(&dream_audio_queue_mutex);

		result = ddvd_dream_alsa_configure(packet->sample_rate,
			packet->channels, packet->passthrough);
		if (result == 0)
			result = ddvd_dream_alsa_write_all(packet->data, packet->size);
		free(packet->data);
		free(packet);
		if (result < 0) {
			pthread_mutex_lock(&dream_audio_queue_mutex);
			dream_audio_queue_failed = 1;
			dream_audio_queue_stop_requested = 1;
			pthread_cond_broadcast(&dream_audio_queue_cond);
			pthread_mutex_unlock(&dream_audio_queue_mutex);
			break;
		}
	}
	return NULL;
}

static int ddvd_dream_audio_queue_start(void)
{
	int result;

	if (dream_audio_queue_running)
		return 0;
	pthread_mutex_lock(&dream_audio_queue_mutex);
	ddvd_dream_audio_queue_clear_locked();
	dream_audio_queue_stop_requested = 0;
	dream_audio_queue_paused = 0;
	dream_audio_queue_failed = 0;
	pthread_mutex_unlock(&dream_audio_queue_mutex);

	result = pthread_create(&dream_audio_queue_thread, NULL,
		ddvd_dream_audio_queue_worker, NULL);
	if (result != 0) {
		errno = result;
		fprintf(stderr, "libdreamdvd: cannot start Dream audio queue: %s\n",
			strerror(errno));
		return -1;
	}
	dream_audio_queue_running = 1;
	return 0;
}

static void ddvd_dream_audio_queue_stop(void)
{
	if (dream_audio_queue_running) {
		pthread_mutex_lock(&dream_audio_queue_mutex);
		dream_audio_queue_stop_requested = 1;
		dream_audio_queue_paused = 0;
		pthread_cond_broadcast(&dream_audio_queue_cond);
		pthread_mutex_unlock(&dream_audio_queue_mutex);
		pthread_join(dream_audio_queue_thread, NULL);
		dream_audio_queue_running = 0;
	}
	pthread_mutex_lock(&dream_audio_queue_mutex);
	ddvd_dream_audio_queue_clear_locked();
	pthread_mutex_unlock(&dream_audio_queue_mutex);
}

static int ddvd_dream_audio_queue_packet(const uint8_t *data, size_t size,
	unsigned int sample_rate, unsigned int channels, int passthrough)
{
	struct ddvd_dream_audio_packet *packet;

	packet = malloc(sizeof(*packet));
	if (packet == NULL)
		return -1;
	packet->data = malloc(size);
	if (packet->data == NULL) {
		free(packet);
		return -1;
	}
	memcpy(packet->data, data, size);
	packet->size = size;
	packet->sample_rate = sample_rate;
	packet->channels = channels;
	packet->passthrough = passthrough;
	packet->next = NULL;

	pthread_mutex_lock(&dream_audio_queue_mutex);
	while (!dream_audio_queue_stop_requested &&
		!dream_audio_queue_failed && dream_audio_queue_bytes != 0 &&
		dream_audio_queue_bytes + size > DREAM_AUDIO_QUEUE_SIZE)
		pthread_cond_wait(&dream_audio_queue_cond,
			&dream_audio_queue_mutex);
	if (dream_audio_queue_stop_requested || dream_audio_queue_failed) {
		pthread_mutex_unlock(&dream_audio_queue_mutex);
		free(packet->data);
		free(packet);
		return -1;
	}
	if (dream_audio_queue_tail != NULL)
		dream_audio_queue_tail->next = packet;
	else
		dream_audio_queue_head = packet;
	dream_audio_queue_tail = packet;
	dream_audio_queue_bytes += size;
	pthread_cond_broadcast(&dream_audio_queue_cond);
	pthread_mutex_unlock(&dream_audio_queue_mutex);
	return (int)size;
}

/* AC-3 frame sizes in 16-bit words for 48, 44.1 and 32 kHz.  DVD private
 * stream PES packets may split a frame at any byte.  dream_decoder_decode()
 * deliberately has no FFmpeg parser because dreamaudiosink normally receives
 * complete frames from GStreamer's ac3parse element, so frame them here. */
static size_t ddvd_dream_ac3_frame_size(const uint8_t *data, size_t size)
{
	static const uint16_t frame_words[19][3] = {
		{ 64,   69,   96 }, { 80,   87,  120 },
		{ 96,  104,  144 }, { 112, 121,  168 },
		{ 128, 139,  192 }, { 160, 174,  240 },
		{ 192, 208,  288 }, { 224, 243,  336 },
		{ 256, 278,  384 }, { 320, 348,  480 },
		{ 384, 417,  576 }, { 448, 487,  672 },
		{ 512, 557,  768 }, { 640, 696,  960 },
		{ 768, 835, 1152 }, { 896, 975, 1344 },
		{ 1024, 1114, 1536 }, { 1152, 1253, 1728 },
		{ 1280, 1393, 1920 }
	};
	unsigned int fscod;
	unsigned int frmsizecod;
	unsigned int words;

	if (size < 6 || data[0] != 0x0b || data[1] != 0x77)
		return 0;
	fscod = data[4] >> 6;
	frmsizecod = data[4] & 0x3f;
	/* bsid values through 10 are AC-3.  Values above that use the E-AC-3
	 * header layout and are not valid on a DVD-Video AC-3 substream. */
	if (fscod == 3 || frmsizecod > 37 || (data[5] >> 3) > 10)
		return 0;

	words = frame_words[frmsizecod >> 1][fscod];
	if (fscod == 1 && (frmsizecod & 1))
		++words;
	return (size_t)words * 2;
}

static void ddvd_dream_audio_output(
	enum ddvd_dream_decoder_output_type type,
	unsigned int sample_rate,
	unsigned int channels,
	const uint8_t *data,
	size_t size,
	int64_t pts,
	void *user_data)
{
	int passthrough = type == DDVD_DREAM_DECODER_OUTPUT_IEC61937;

	(void)pts;
	(void)user_data;
	if (dream_alsa == NULL || data == NULL || size == 0)
		return;

	/* Keep decoded audio off the dvdnav thread.  Together with the video queue
	 * this gives dvdnav's VOBU read-ahead enough time to build a reserve that
	 * masks short optical-drive seeks and retries. */
	ddvd_dream_audio_queue_packet(data, size, sample_rate, channels,
		passthrough);
}

static int ddvd_dream_alsa_symbol(void **symbol, const char *name)
{
	dlerror();
	*symbol = dlsym(dream_audio_handle, name);
	const char *error = dlerror();

	if (error == NULL)
		return 0;
	fprintf(stderr, "libdreamdvd: cannot load %s: %s\n", name, error);
	return -1;
}

int ddvd_dream_alsa_open(void)
{
	if (dream_alsa != NULL)
		return 0;

	dream_audio_handle = dlopen(DREAM_AUDIO_SINK, RTLD_NOW | RTLD_LOCAL);
	if (dream_audio_handle == NULL) {
		fprintf(stderr, "libdreamdvd: cannot load %s: %s\n",
			DREAM_AUDIO_SINK, dlerror());
		return -1;
	}

#define LOAD_DREAM_ALSA(name) \
	if (ddvd_dream_alsa_symbol((void **)&dream_##name, "dream_" #name) < 0) \
		goto error

	LOAD_DREAM_ALSA(alsa_new);
	LOAD_DREAM_ALSA(alsa_free);
	LOAD_DREAM_ALSA(alsa_set_params);
	LOAD_DREAM_ALSA(alsa_set_nonblocking);
	LOAD_DREAM_ALSA(alsa_write);
	LOAD_DREAM_ALSA(alsa_drop);
	LOAD_DREAM_ALSA(alsa_reset_anchor);
	LOAD_DREAM_ALSA(decoder_new);
	LOAD_DREAM_ALSA(decoder_free);
	LOAD_DREAM_ALSA(decoder_decode);

#undef LOAD_DREAM_ALSA

	dream_alsa = dream_alsa_new("dreamhdmi");
	if (dream_alsa == NULL) {
		fprintf(stderr, "libdreamdvd: cannot create Dream ALSA output\n");
		goto error;
	}
	/* Dream's dmix slave can remain RUNNING with an impossible negative delay.
	 * The plugin detects and repairs that state only in non-blocking mode. */
	dream_alsa_set_nonblocking(dream_alsa, 1);
	dream_alsa_configured = 0;
	dream_alsa_rate = 0;
	dream_alsa_channels = 0;
	dream_alsa_passthrough = 0;
	if (ddvd_dream_audio_queue_start() < 0)
		goto error;
	return 0;

error:
	ddvd_dream_alsa_close();
	return -1;
}

static int ddvd_dream_decoder_prepare(int codec_id, const char *name)
{
	if (dream_decoder != NULL && dream_decoder_codec_id == codec_id)
		return 0;
	if (dream_decoder != NULL) {
		dream_decoder_free(dream_decoder);
		dream_decoder = NULL;
		dream_decoder_codec_id = -1;
	}

	dream_decoder = dream_decoder_new(codec_id, 48000, 2,
		NULL, 0, ddvd_dream_audio_output, NULL);
	if (dream_decoder == NULL) {
		fprintf(stderr, "libdreamdvd: cannot create Dream %s decoder\n", name);
		return -1;
	}
	dream_decoder_codec_id = codec_id;
	return 0;
}

int ddvd_dream_audio_decode_ac3(const uint8_t *data, size_t size, int64_t pts)
{
	size_t offset = 0;
	int result = 0;

	if (data == NULL || size == 0)
		return 0;
	if (size > DREAM_COMPRESSED_BUFFER_SIZE - dream_ac3_buffer_size) {
		/* A normal DVD PES packet is much smaller.  On corrupt input discard
		 * the partial frame and resynchronise instead of overflowing. */
		dream_ac3_buffer_size = 0;
	}
	if (size > DREAM_COMPRESSED_BUFFER_SIZE)
		return -1;
	if (dream_alsa == NULL && ddvd_dream_alsa_open() < 0)
		return -1;
	if (ddvd_dream_decoder_prepare(DDVD_AV_CODEC_ID_AC3, "AC3") < 0)
		return -1;

	memcpy(dream_ac3_buffer + dream_ac3_buffer_size, data, size);
	dream_ac3_buffer_size += size;

	while (dream_ac3_buffer_size - offset >= 6) {
		size_t frame_size;

		if (dream_ac3_buffer[offset] != 0x0b ||
			dream_ac3_buffer[offset + 1] != 0x77) {
			++offset;
			continue;
		}
		frame_size = ddvd_dream_ac3_frame_size(
			dream_ac3_buffer + offset, dream_ac3_buffer_size - offset);
		if (frame_size == 0) {
			++offset;
			continue;
		}
		if (dream_ac3_buffer_size - offset < frame_size)
			break;

		result = dream_decoder_decode(dream_decoder,
			dream_ac3_buffer + offset, (int)frame_size, pts, pts);
		if (result < 0) {
			/* A valid-looking sync word can occur in damaged payload.  Move one
			 * byte and search for the next frame instead of replaying it. */
			++offset;
			continue;
		}
		offset += frame_size;
	}

	if (offset != 0) {
		memmove(dream_ac3_buffer, dream_ac3_buffer + offset,
			dream_ac3_buffer_size - offset);
		dream_ac3_buffer_size -= offset;
	}
	/* If no complete header remains, retain only a possible first sync byte. */
	if (dream_ac3_buffer_size > 1 &&
		!(dream_ac3_buffer[0] == 0x0b && dream_ac3_buffer[1] == 0x77)) {
		dream_ac3_buffer[0] =
			dream_ac3_buffer[dream_ac3_buffer_size - 1] == 0x0b ? 0x0b : 0;
		dream_ac3_buffer_size = dream_ac3_buffer[0] == 0x0b ? 1 : 0;
	}
	return result;
}

static int ddvd_dream_dts_sync(const uint8_t *data, size_t size)
{
	if (size < 4)
		return 0;
	return (data[0] == 0x7f && data[1] == 0xfe &&
		data[2] == 0x80 && data[3] == 0x01) ||
		(data[0] == 0xfe && data[1] == 0x7f &&
		data[2] == 0x01 && data[3] == 0x80);
}

/* DVD-Video carries DTS core frames in 16-bit big-endian form.  Accept the
 * byte-swapped form too because some authored discs use it.  PES packets can
 * start or end in the middle of a frame, so return the complete core-frame
 * size and let the caller retain incomplete data. */
static size_t ddvd_dream_dts_frame_size(const uint8_t *data, size_t size)
{
	uint8_t header[8];
	const uint8_t *h = data;
	size_t frame_size;

	if (size < sizeof(header) || !ddvd_dream_dts_sync(data, size))
		return 0;
	if (data[0] == 0xfe) {
		size_t i;
		for (i = 0; i < sizeof(header); i += 2) {
			header[i] = data[i + 1];
			header[i + 1] = data[i];
		}
		h = header;
	}

	frame_size = (size_t)(((h[5] & 0x03) << 12) |
		(h[6] << 4) | (h[7] >> 4)) + 1;
	/* DTS core frames on DVD are bounded well below this buffer. */
	if (frame_size < 96 || frame_size > 32768)
		return 0;
	return frame_size;
}

static size_t ddvd_dream_dts_sync_suffix(const uint8_t *data, size_t size)
{
	static const uint8_t sync_be[4] = { 0x7f, 0xfe, 0x80, 0x01 };
	static const uint8_t sync_le[4] = { 0xfe, 0x7f, 0x01, 0x80 };
	size_t keep;

	for (keep = size < 3 ? size : 3; keep > 0; --keep) {
		if (!memcmp(data + size - keep, sync_be, keep) ||
			!memcmp(data + size - keep, sync_le, keep))
			return keep;
	}
	return 0;
}

int ddvd_dream_audio_decode_dts(const uint8_t *data, size_t size, int64_t pts)
{
	size_t offset = 0;
	int result = 0;

	if (data == NULL || size == 0)
		return 0;
	if (size > DREAM_COMPRESSED_BUFFER_SIZE - dream_dts_buffer_size)
		dream_dts_buffer_size = 0;
	if (size > DREAM_COMPRESSED_BUFFER_SIZE)
		return -1;
	if (dream_alsa == NULL && ddvd_dream_alsa_open() < 0)
		return -1;
	if (ddvd_dream_decoder_prepare(DDVD_AV_CODEC_ID_DTS, "DTS") < 0)
		return -1;

	memcpy(dream_dts_buffer + dream_dts_buffer_size, data, size);
	dream_dts_buffer_size += size;

	while (dream_dts_buffer_size - offset >= 8) {
		size_t frame_size;

		if (!ddvd_dream_dts_sync(dream_dts_buffer + offset,
			dream_dts_buffer_size - offset)) {
			++offset;
			continue;
		}
		frame_size = ddvd_dream_dts_frame_size(dream_dts_buffer + offset,
			dream_dts_buffer_size - offset);
		if (frame_size == 0) {
			++offset;
			continue;
		}
		if (dream_dts_buffer_size - offset < frame_size)
			break;

		result = dream_decoder_decode(dream_decoder,
			dream_dts_buffer + offset, (int)frame_size, pts, pts);
		if (result < 0) {
			++offset;
			continue;
		}
		offset += frame_size;
	}

	if (offset != 0) {
		memmove(dream_dts_buffer, dream_dts_buffer + offset,
			dream_dts_buffer_size - offset);
		dream_dts_buffer_size -= offset;
	}
	if (dream_dts_buffer_size != 0 &&
		!ddvd_dream_dts_sync(dream_dts_buffer, dream_dts_buffer_size)) {
		size_t keep = ddvd_dream_dts_sync_suffix(
			dream_dts_buffer, dream_dts_buffer_size);
		if (keep != 0)
			memmove(dream_dts_buffer,
				dream_dts_buffer + dream_dts_buffer_size - keep, keep);
		dream_dts_buffer_size = keep;
	}
	return result;
}

int ddvd_dream_alsa_write_pcm(const int16_t *data, size_t size, int64_t pts)
{
	(void)pts;

	if (data == NULL || size == 0)
		return 0;
	if (dream_alsa == NULL && ddvd_dream_alsa_open() < 0)
		return -1;
	return ddvd_dream_audio_queue_packet((const uint8_t *)data, size,
		48000, 2, 0);
}

void ddvd_dream_alsa_reset(void)
{
	dream_ac3_buffer_size = 0;
	dream_dts_buffer_size = 0;
	if (dream_alsa == NULL)
		return;
	ddvd_dream_audio_queue_stop();
	dream_alsa_drop(dream_alsa);
	dream_alsa_reset_anchor(dream_alsa);
	if (ddvd_dream_audio_queue_start() < 0)
		fprintf(stderr, "libdreamdvd: cannot restart Dream audio queue\n");
}

void ddvd_dream_alsa_pause(void)
{
	pthread_mutex_lock(&dream_audio_queue_mutex);
	dream_audio_queue_paused = 1;
	pthread_mutex_unlock(&dream_audio_queue_mutex);
}

void ddvd_dream_alsa_resume(void)
{
	pthread_mutex_lock(&dream_audio_queue_mutex);
	dream_audio_queue_paused = 0;
	pthread_cond_broadcast(&dream_audio_queue_cond);
	pthread_mutex_unlock(&dream_audio_queue_mutex);
}

void ddvd_dream_alsa_close(void)
{
	dream_ac3_buffer_size = 0;
	dream_dts_buffer_size = 0;
	ddvd_dream_audio_queue_stop();
	if (dream_decoder != NULL && dream_decoder_free != NULL)
		dream_decoder_free(dream_decoder);
	dream_decoder = NULL;
	dream_decoder_codec_id = -1;

	if (dream_alsa != NULL && dream_alsa_free != NULL)
		dream_alsa_free(dream_alsa);
	dream_alsa = NULL;
	dream_alsa_configured = 0;

	if (dream_audio_handle != NULL)
		dlclose(dream_audio_handle);
	dream_audio_handle = NULL;
	dream_alsa_new = NULL;
	dream_alsa_free = NULL;
	dream_alsa_set_params = NULL;
	dream_alsa_set_nonblocking = NULL;
	dream_alsa_write = NULL;
	dream_alsa_drop = NULL;
	dream_alsa_reset_anchor = NULL;
	dream_decoder_new = NULL;
	dream_decoder_free = NULL;
	dream_decoder_decode = NULL;
}
