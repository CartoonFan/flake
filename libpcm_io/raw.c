/**
 * libpcm_io: Raw PCM Audio I/O library
 * Copyright (c) 2006-2007 Justin Ruggles
 *
 * libpcm_io is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * libpcm_io is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with libpcm_io; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file raw.c
 * Raw file format
 */

#include "pcm_io_common.h"
#include "pcm_io.h"

static int
raw_probe(uint8_t *data, int size)
{
    if(data == NULL || size < 0)
        return 0;
    return 1;
}

static int
raw_init(PcmFile *pf)
{
    pcmfile_set_source_params(pf, 2, PCM_SAMPLE_FMT_S16, PCM_BYTE_ORDER_LE,
                              44100);

    pf->data_size = 0;
    pf->data_start = 0;
    if(pf->seekable && pf->file_size > 0) {
        if(pf->data_size > 0)
            pf->data_size = MIN(pf->data_size, pf->file_size - pf->data_start);
        else
            pf->data_size = pf->file_size - pf->data_start;
    }
    pf->samples = (pf->data_size / pf->block_align);
    pf->read_to_eof = 1;

    return 0;
}

PcmFormat raw_format = {
    "raw",
    "Raw PCM",
    PCM_FORMAT_RAW,
    raw_probe,
    raw_init,
    NULL
};
