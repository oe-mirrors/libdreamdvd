#ifndef __DREAMALSA_H__
#define __DREAMALSA_H__

#include <stddef.h>
#include <stdint.h>

int ddvd_dream_alsa_open(void);
int ddvd_dream_alsa_write_pcm(const int16_t *data, size_t size, int64_t pts);
int ddvd_dream_audio_decode_ac3(const uint8_t *data, size_t size, int64_t pts);
int ddvd_dream_audio_decode_dts(const uint8_t *data, size_t size, int64_t pts);
void ddvd_dream_alsa_reset(void);
void ddvd_dream_alsa_close(void);

#endif
