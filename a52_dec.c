/* 
 * DreamDVD V0.9 - DVD-Player for Dreambox
 * Copyright (C) 2007 by Seddi
 * 
 * This DVD Player is based upon the great work from the libdvdnav project,
 * a52dec library, ffmpeg and the knowledge from all the people who made 
 * watching DVD within linux possible.
 * 
 * DreamDVD is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * DreamDVD is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 *
 * part of libdreamdvd
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <inttypes.h>
#include <dlfcn.h>


#include "a52dec.h"

__typeof__(a52_init) *ddvd_a52_init;
__typeof__(a52_samples) *ddvd_a52_samples;
__typeof__(a52_syncinfo) *ddvd_a52_syncinfo;
__typeof__(a52_frame) *ddvd_a52_frame;
__typeof__(a52_block) *ddvd_a52_block;
__typeof__(a52_free) *ddvd_a52_free;

void *a52_handle;

a52_state_t * state;


// try to dynamically load and wrap liba52.so.0

int ddvd_load_liba52(void)
{
	a52_handle = dlopen("liba52.so.0", RTLD_NOW | RTLD_LOCAL);
	if (!a52_handle) {
		printf("libdreamdvd: soft ac3 decoding is not available: %s\n", dlerror());
		return 0;
	}

#define LOAD_A52_SYMBOL(name) do { \
	dlerror(); \
	ddvd_##name = (__typeof__(ddvd_##name))dlsym(a52_handle, #name); \
	const char *error = dlerror(); \
	if (error != NULL) { \
		printf("libdreamdvd: cannot load liba52 symbol %s: %s\n", #name, error); \
		goto error; \
	} \
} while (0)

	LOAD_A52_SYMBOL(a52_init);
	LOAD_A52_SYMBOL(a52_samples);
	LOAD_A52_SYMBOL(a52_syncinfo);
	LOAD_A52_SYMBOL(a52_frame);
	LOAD_A52_SYMBOL(a52_block);
	LOAD_A52_SYMBOL(a52_free);

#undef LOAD_A52_SYMBOL

	printf("libdreamdvd: soft ac3 decoding is available, liba52.so.0 loaded\n");
	return 1;

error:
	ddvd_close_liba52();
	return 0;
}

// close dynamically loaded liba52.so.0

void ddvd_close_liba52(void)
{
	if (a52_handle != NULL)
		dlclose(a52_handle);
	a52_handle = NULL;
	ddvd_a52_init = NULL;
	ddvd_a52_samples = NULL;
	ddvd_a52_syncinfo = NULL;
	ddvd_a52_frame = NULL;
	ddvd_a52_block = NULL;
	ddvd_a52_free = NULL;
}

// convert normalized liba52 samples to signed 16bit PCM

static inline int16_t a52_convert(sample_t sample)
{
	if (!isfinite(sample))
		return 0;
	if (sample >= 1.0)
		return INT16_MAX;
	if (sample <= -1.0)
		return INT16_MIN;
	return (int16_t)lrint((double)sample * 32768.0);
}

// liba52 gives us 256 samples left - 256 samples right
// we need 1 left - 1 right so lets sort them

static void a52_convert2s16_2 (sample_t * _f, int16_t * s16)
{
    int i;

    for (i = 0; i < 256; i++) {
	s16[2*i] = a52_convert (_f[i]);
	s16[2*i+1] = a52_convert (_f[i+256]);
    }
}

// a52 decode function (needs liba52)

int ddvd_ac3_decode(const uint8_t *input, unsigned int len, int16_t *output)
{
	static int sample_rate;
    static int flags;
    int bit_rate;
	int out_len=0;
	const uint8_t *end; 
	end=input+len;
	
    static uint8_t buf[3840];
    static uint8_t * bufptr = buf;
    static uint8_t * bufpos = buf + 7;

    while (1) {
	len = end - input;
	if (!len)
	    break;
	if (len > bufpos - bufptr)
	    len = bufpos - bufptr;
	
	memcpy (bufptr, input, len);
	bufptr += len;
	input += len;

	if (bufptr == bufpos) {
	    if (bufpos == buf + 7) {
		int length;

		length = ddvd_a52_syncinfo (buf, &flags, &sample_rate, &bit_rate);
		if (!length) {
		    for (bufptr = buf; bufptr < buf + 6; bufptr++)
			bufptr[0] = bufptr[1];
		    continue;
		}
		bufpos = buf + length;
	    } else {
		sample_t level;
		sample_t bias;
		int i;

		flags=A52_DOLBY|A52_ADJUST_LEVEL;
			
		bias = 0.0;
		level = 1.0;

		if (ddvd_a52_frame (state, buf, &flags, &level, bias))
		    goto error;

		for (i = 0; i < 6; i++) {
		    if (ddvd_a52_block (state))
			goto error;
			a52_convert2s16_2(ddvd_a52_samples(state),output);
			output+=512;
			out_len+=1024;
		}
		bufptr = buf;
		bufpos = buf + 7;
		continue;
	    error:
		bufptr = buf;
		bufpos = buf + 7;
	    }
	}
    }
	return out_len;
}
