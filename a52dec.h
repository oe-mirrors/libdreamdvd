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

#ifndef __A52DEC_H__

#define __A52DEC_H__

#include <stdint.h>
#include <a52dec/a52.h>

/* Keep the dynamically loaded function signatures tied to the installed
 * liba52 headers.  In particular, sample_t is float in the normal liba52
 * build.  Declaring it as an integer changes the calling convention on
 * AArch64 and corrupts the decoded samples on every architecture. */
extern __typeof__(a52_init) *ddvd_a52_init;
extern __typeof__(a52_samples) *ddvd_a52_samples;
extern __typeof__(a52_syncinfo) *ddvd_a52_syncinfo;
extern __typeof__(a52_frame) *ddvd_a52_frame;
extern __typeof__(a52_block) *ddvd_a52_block;
extern __typeof__(a52_free) *ddvd_a52_free;

extern void *a52_handle;

extern a52_state_t * state;

int ddvd_ac3_decode(const uint8_t *input, unsigned int len, int16_t *output);
int ddvd_load_liba52(void);
void ddvd_close_liba52(void);

#endif
