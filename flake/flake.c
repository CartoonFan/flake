/**
 * Flake: FLAC audio encoder
 * Copyright (c) 2006-2007 Justin Ruggles
 *
 * Flake is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Flake is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Flake; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "common.h"

#include <limits.h>

/* used for binary mode piped i/o on Windows */
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "bswap.h"
#include "pcm_io.h"
#include "flake.h"

#ifndef PATH_MAX
#define PATH_MAX 255
#endif

static void
print_usage(FILE *out)
{
    fprintf(out, "usage: flake [options] <input.wav> [-o output.flac]\n"
                 "type 'flake -h' for more details.\n\n");
}

static void
print_help(FILE *out)
{
    fprintf(out, "usage: flake [options] <input.wav> [-o output.flac]\n"
                 "options:\n"
                 "       [-h]         Print out list of commandline options\n"
                 "       [-q]         Quiet mode: no console output\n"
                 "       [-p #]       Padding bytes to put in header (default: 8192)\n"
                 "       [-0 ... -12] Compression level (default: 5)\n"
                 "                        0 = -b 1152 -t 1 -l 2,2 -m 0 -r 3 -s 0\n"
                 "                        1 = -b 1152 -t 1 -l 2,4 -m 1 -r 3 -s 1\n"
                 "                        2 = -b 1152 -t 1 -l 4   -m 1 -r 3 -s 1\n"
                 "                        3 = -b 4096 -t 2 -l 6   -m 1 -r 4 -s 0\n"
                 "                        4 = -b 4096 -t 2 -l 8   -m 1 -r 4 -s 1\n"
                 "                        5 = -b 4096 -t 2 -l 8   -m 1 -r 5 -s 1\n"
                 "                        6 = -b 4096 -t 2 -l 8   -m 2 -r 6 -s 1\n"
                 "                        7 = -b 4096 -t 2 -l 8   -m 3 -r 6 -s 1\n"
                 "                        8 = -b 4096 -t 2 -l 12  -m 6 -r 6 -s 1\n"
                 "                        9 = -b 4096 -t 2 -l 12  -m 6 -r 8 -s 1 -v 1\n"
                 "                       10 = -b 4096 -t 2 -l 12  -m 5 -r 8 -s 1 -v 1\n"
                 "                       11 = -b 8192 -t 2 -l 32  -m 6 -r 8 -s 1 -v 1\n"
                 "                       12 = -b 8192 -t 2 -l 32  -m 5 -r 8 -s 1 -v 1\n"
                 "       [-b #]       Block size [16 - 65535] (default: 4096)\n"
                 "       [-t #]       Prediction type\n"
                 "                        0 = no prediction / verbatim\n"
                 "                        1 = fixed prediction\n"
                 "                        2 = Levinson-Durbin recursion (default)\n"
                 "       [-l #[,#]]   Prediction order {max} or {min},{max} (default: 1,5)\n"
                 "       [-m #]       Prediction order selection method\n"
                 "                        0 = maximum\n"
                 "                        1 = estimate (default)\n"
                 "                        2 = 2-level\n"
                 "                        3 = 4-level\n"
                 "                        4 = 8-level\n"
                 "                        5 = full search\n"
                 "                        6 = log search\n"
                 "       [-r #[,#]]   Rice partition order {max} or {min},{max} (default: 0,5)\n"
                 "       [-s #]       Stereo decorrelation method\n"
                 "                        0 = independent L+R channels\n"
                 "                        1 = mid-side (default)\n"
                 "       [-v #]       Variable block size\n"
                 "                        0 = fixed (default)\n"
                 "                        1 = variable\n"
                 "\n");
}

typedef struct FilePair {
    char *infile;
    char *outfile;
    FILE *ifp;
    FILE *ofp;
} FilePair;

typedef struct CommandOptions {
    FilePair *filelist;
    int input_count;
    int found_output;
    int compr;
    int omethod;
    int ptype;
    int omin;
    int omax;
    int pomin;
    int pomax;
    int bsize;
    int stmethod;
    int padding;
    int vbs;
    int quiet;
} CommandOptions;

static int
parse_number(char *arg, int max) {
    int i;
    int m = 0;
    int n = 0;
    int digits;
    for(i=0; i<max; i++) {
        if(arg[i] == '\0') break;
        if(m == 0) m = 1;
        else m *= 10;
    }
    if(arg[i] != '\0') return -1;
    digits = i;
    for(i=0; i<digits; i++) {
        if(arg[i] < '0' || arg[i] > '9') {
            fprintf(stderr, "invalid digit: %c (ASCII:0x%02X)\n", arg[i], arg[i]);
            return -1;
        }
        n += (arg[i]-48) * m;
        m /= 10;
    }
    return n;
}

static int
parse_commandline(int argc, char **argv, CommandOptions *opts)
{
    int i;
    static const char *param_str = "bhlmopqrstv";
    int max_digits = 8;
    int ifc = 0;

    opts->filelist = NULL;
    if(argc < 2) {
        return 1;
    }

    opts->filelist = calloc(argc * sizeof(FilePair), 1);
    opts->input_count = 0;
    opts->found_output = 0;
    opts->compr = 5;
    opts->omethod = -1;
    opts->ptype = -1;
    opts->omin = -1;
    opts->omax = -1;
    opts->pomin = -1;
    opts->pomax = -1;
    opts->bsize = -1;
    opts->stmethod = -1;
    opts->padding = -1;
    opts->vbs = -1;
    opts->quiet = 0;

    for(i=1; i<argc; i++) {
        if(argv[i][0] == '-' && argv[i][1] != '\0') {
            if(argv[i][1] >= '0' && argv[i][1] <= '9') {
                if(argv[i][2] != '\0' && argv[i][3] != '\0') {
                    opts->filelist[ifc].infile = argv[i];
                    ifc++;
                } else {
                    opts->compr = parse_number(&argv[i][1], max_digits);
                    if(opts->compr < 0) return 1;
                }
            } else {
                // if argument starts with '-' and is more than 1 char, treat
                // it as a filename
                if(argv[i][2] != '\0') {
                    opts->filelist[ifc].infile = argv[i];
                    ifc++;
                    continue;
                }
                // check to see if param is valid
                if(strchr(param_str, argv[i][1]) == NULL) {
                    fprintf(stderr, "invalid option: -%c\n", argv[i][1]);
                    return 1;
                }
                // print commandline help
                if(argv[i][1] == 'h') {
                    return 2;
                }
                i++;
                if(i >= argc) {
                    fprintf(stderr, "incomplete option: -%c\n", argv[i-1][1]);
                    return 1;
                }

                switch(argv[i-1][1]) {
                    case 'b':
                        opts->bsize = parse_number(argv[i], max_digits);
                        if(opts->bsize < 0) return 1;
                        break;
                    case 'l':
                        if(strchr(argv[i], ',') == NULL) {
                            opts->omax = parse_number(argv[i], max_digits);
                            if(opts->omax < 0) return 1;
                        } else {
                            char *po = strchr(argv[i], ',');
                            po[0] = '\0';
                            opts->omin = parse_number(argv[i], max_digits);
                            if(opts->omin < 0) return 1;
                            opts->omax = parse_number(&po[1], max_digits);
                            if(opts->omax < 0) return 1;
                        }
                        break;
                    case 'm':
                        opts->omethod = parse_number(argv[i], max_digits);
                        if(opts->omethod < 0) return 1;
                        break;
                    case 'o':
                        if(opts->found_output) {
                            return 1;
                        } else {
                            int olen = strnlen(argv[i], PATH_MAX) + 1;
                            opts->filelist[0].outfile = calloc(1, olen+5);
                            strncpy(opts->filelist[0].outfile, argv[i], olen);
                            opts->found_output = 1;
                        }
                        break;
                    case 'p':
                        opts->padding = parse_number(argv[i], max_digits);
                        if(opts->padding < 0) return 1;
                        break;
                    case 'q':
                        i--;
                        opts->quiet = 1;
                        break;
                    case 'r':
                        if(strchr(argv[i], ',') == NULL) {
                            opts->pomin = 0;
                            opts->pomax = parse_number(argv[i], max_digits);
                            if(opts->pomax < 0) return 1;
                        } else {
                            char *po = strchr(argv[i], ',');
                            po[0] = '\0';
                            opts->pomin = parse_number(argv[i], max_digits);
                            if(opts->pomin < 0) return 1;
                            opts->pomax = parse_number(&po[1], max_digits);
                            if(opts->pomax < 0) return 1;
                        }
                        break;
                    case 's':
                        opts->stmethod = parse_number(argv[i], max_digits);
                        if(opts->stmethod < 0) return 1;
                        break;
                    case 't':
                        opts->ptype = parse_number(argv[i], max_digits);
                        if(opts->ptype < 0) return 1;
                        break;
                    case 'v':
                        opts->vbs = parse_number(argv[i], max_digits);
                        if(opts->vbs < 0) return 1;
                        break;
                }
            }
        } else {
            // if argument does not start with '-' parse as a filename. also,
            // if the argument is a single '-' treat it as a filename
            opts->filelist[ifc].infile = argv[i];
            ifc++;
        }
    }
    if(!ifc) {
        fprintf(stderr, "error parsing filenames.\n");
        return 1;
    }
    if(opts->found_output && ifc > 1) {
        fprintf(stderr, "cannot specify output file when using multiple input files\n");
        return 1;
    }
    if(!opts->found_output) {
        // if no output is specified, use input filename with .flac extension
        for(i=0; i<ifc; i++) {
            int ext = strnlen(opts->filelist[i].infile, PATH_MAX);
            opts->filelist[i].outfile = calloc(1, ext+6);
            strncpy(opts->filelist[i].outfile, opts->filelist[i].infile, ext+1);
            opts->filelist[i].outfile[ext] = '\0';
            while(ext > 0 && opts->filelist[i].outfile[ext] != '.') ext--;
            if(ext >= (PATH_MAX-5)) {
                fprintf(stderr, "input filename too long\n");
                return 1;
            }
            strncpy(&opts->filelist[i].outfile[ext], ".flac", 6);
        }
    }

    // disallow infile & outfile with same name except with piping
    for(i=0; i<ifc; i++) {
        if(strncmp(opts->filelist[i].infile, "-", 2) && strncmp(opts->filelist[i].outfile, "-", 2)) {
            if(!strcmp(opts->filelist[i].infile, opts->filelist[i].outfile)) {
                fprintf(stderr, "output filename cannot match input filename\n");
                return 1;
            }
        }
    }

    opts->input_count = ifc;
    return 0;
}

static void
print_params(FlakeContext *s)
{
    char *omethod_s, *stmethod_s, *ptype_s;

    fprintf(stderr, "variable block size: %s\n", s->params.variable_block_size?"yes":"no");
    ptype_s = "ERROR";
    switch(s->params.prediction_type) {
        case 0: ptype_s = "none (verbatim mode)";  break;
        case 1: ptype_s = "fixed";  break;
        case 2: ptype_s = "levinson-durbin"; break;
    }
    fprintf(stderr, "prediction type: %s\n", ptype_s);
    if(s->params.prediction_type != FLAKE_PREDICTION_NONE) {
        fprintf(stderr, "prediction order: %d,%d\n", s->params.min_prediction_order,
                                                     s->params.max_prediction_order);
        fprintf(stderr, "partition order: %d,%d\n", s->params.min_partition_order,
                                                    s->params.max_partition_order);
        omethod_s = "ERROR";
        switch(s->params.order_method) {
            case 0: omethod_s = "maximum";  break;
            case 1: omethod_s = "estimate"; break;
            case 2: omethod_s = "2-level"; break;
            case 3: omethod_s = "4-level"; break;
            case 4: omethod_s = "8-level"; break;
            case 5: omethod_s = "full search";   break;
            case 6: omethod_s = "log search";  break;
        }
        fprintf(stderr, "order method: %s\n", omethod_s);
    }
    if(s->channels == 2) {
        stmethod_s = "ERROR";
        switch(s->params.stereo_method) {
            case 0: stmethod_s = "independent";  break;
            case 1: stmethod_s = "mid-side";     break;
        }
        fprintf(stderr, "stereo method: %s\n", stmethod_s);
    }
    fprintf(stderr, "header padding: %d\n", s->params.padding_size);
}

static int
encode_file(CommandOptions *opts, FilePair *files, int first_file)
{
    FlakeContext s;
    PcmFile wf;
    int header_size, subset;
    uint8_t *frame;
    int16_t *wav;
    int percent;
    int fs;
    uint32_t nr, samplecount, bytecount;
    float kb, sec, kbps, wav_bytes;

    if(pcmfile_init(&wf, files->ifp, PCM_SAMPLE_FMT_S16, PCM_FORMAT_UNKNOWN)) {
        fprintf(stderr, "invalid input file: %s\n", files->infile);
        return 1;
    }

    // set parameters from input audio
    s.channels = wf.channels;
    s.sample_rate = wf.sample_rate;
    s.bits_per_sample = 16;
    // TODO: FLAC supports values up to 36-bits for number of samples, but libflake only uses a 32-bit value
    s.samples = MIN(wf.samples, UINT32_MAX);

    // set parameters from commandline
    s.params.compression = opts->compr;
    if(flake_set_defaults(&s.params)) {
        return 1;
    }
    if(opts->bsize    >= 0) s.params.block_size           = opts->bsize;
    if(opts->omethod  >= 0) s.params.order_method         = opts->omethod;
    if(opts->stmethod >= 0) s.params.stereo_method        = opts->stmethod;
    if(opts->ptype    >= 0) s.params.prediction_type      = opts->ptype;
    if(opts->omin >= 0 || opts->omax >= 0) {
        // commandline arguments are such that you can either specify both
        // minimum and maximum or just maximum.  if just maximum is specified,
        // the default minimum depends on the prediction type.
        assert(opts->omax >= 0);
        s.params.max_prediction_order = opts->omax;
        if(opts->omin >= 0) {
            s.params.min_prediction_order = opts->omin;
        } else {
            if(s.params.prediction_type == FLAKE_PREDICTION_LEVINSON)
                s.params.min_prediction_order = 1;
            else
                s.params.min_prediction_order = 0;
        }
    }
    if(opts->pomin    >= 0) s.params.min_partition_order  = opts->pomin;
    if(opts->pomax    >= 0) s.params.max_partition_order  = opts->pomax;
    if(opts->padding  >= 0) s.params.padding_size         = opts->padding;
    if(opts->vbs      >= 0) s.params.variable_block_size  = opts->vbs;

    subset = flake_validate_params(&s);
    if(subset < 0) {
        fprintf(stderr, "Error: invalid encoding parameters.\n");
        return 1;
    }

    // initialize encoder
    header_size = flake_encode_init(&s);
    if(header_size < 0) {
        flake_encode_close(&s);
        fprintf(stderr, "Error initializing encoder.\n");
        return 1;
    }
    fwrite(s.header, 1, header_size, files->ofp);

    // print encoding parameters
    if(first_file && !opts->quiet) {
        if(subset == 1) {
            fprintf(stderr,"=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n"
                           " WARNING! The chosen encoding options are\n"
                           " not FLAC Subset compliant. Therefore, the\n"
                           " encoded file(s) may not work properly with\n"
                           " some FLAC players and decoders.\n"
                           "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n\n");
        }
        fprintf(stderr, "block size: %d\n", s.params.block_size);
        print_params(&s);
    }

    if(!opts->quiet) {
        fprintf(stderr, "\n");
        fprintf(stderr, "input file:  \"%s\"\n", files->infile);
        fprintf(stderr, "output file: \"%s\"\n", files->outfile);
        pcmfile_print(&wf, stderr);
        if(wf.bit_width != 16) {
            fprintf(stderr, "WARNING! converting to 16-bit (not lossless)\n");
        }
        if(wf.samples > 0) {
            int64_t tms;
            int th, tm, ts;
            tms = (int64_t)(wf.samples * 1000.0 / wf.sample_rate);
            ts = tms / 1000;
            tms = tms % 1000;
            tm = ts / 60;
            ts = ts % 60;
            th = tm / 60;
            tm = tm % 60;
            fprintf(stderr, "samples: %llu (", wf.samples);
            if(th) fprintf(stderr, "%dh", th);
            fprintf(stderr, "%dm", tm);
            fprintf(stderr, "%d.%03ds)\n", ts, (int)tms);
        } else {
            fprintf(stderr, "samples: unknown\n");
        }
        fprintf(stderr, "\n");
    }

    frame = flake_get_buffer(&s);
    wav = malloc(s.params.block_size * wf.channels * sizeof(int16_t));

    samplecount = percent = 0;
    wav_bytes = 0;
    bytecount = header_size;
    nr = pcmfile_read_samples(&wf, wav, s.params.block_size);
    while(nr > 0) {
        fs = flake_encode_frame(&s, wav, nr);
        if(fs < 0) {
            fprintf(stderr, "\nError encoding frame\n");
        } else if(fs > 0) {
            fwrite(frame, 1, fs, files->ofp);
            samplecount = MAX(samplecount, samplecount+nr);
            if(!opts->quiet) {
                bytecount += fs;
                kb = ((bytecount * 8.0) / 1000.0);
                sec = ((float)samplecount) / ((float)s.sample_rate);
                if(samplecount > 0) kbps = kb / sec;
                else kbps = kb;
                if(s.samples > 0) {
                    percent = ((samplecount * 100.5) / s.samples);
                }
                wav_bytes = samplecount*wf.block_align;
                if(!opts->quiet) {
                    fprintf(stderr, "\rprogress: %3d%% | ratio: %1.3f | "
                                    "bitrate: %4.1f kbps ",
                            percent, (bytecount / wav_bytes), kbps);
                }
            }
        }
        nr = pcmfile_read_samples(&wf, wav, s.params.block_size);
    }
    if(!opts->quiet) {
        fprintf(stderr, "| bytes: %d \n\n", bytecount);
    }

    // if seeking is possible, rewrite streaminfo metadata header
    if(!fseek(files->ofp, 8, SEEK_SET)) {
        FlakeStreaminfo strminfo;
        if(!flake_metadata_get_streaminfo(&s, &strminfo)) {
            uint8_t strminfo_data[34];
            flake_metadata_write_streaminfo(&strminfo, strminfo_data);
            fwrite(strminfo_data, 1, 34, files->ofp);
        }
    }

    pcmfile_close(&wf);
    flake_encode_close(&s);
    free(wav);

    return 0;
}

static int
open_files(FilePair *files)
{
    if(!strncmp(files->infile, "-", 2)) {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        files->ifp = stdin;
    } else {
        files->ifp = fopen(files->infile, "rb");
        if(!files->ifp) {
            fprintf(stderr, "error opening input file: %s\n", files->infile);
            return 1;
        }
    }
    if(!strncmp(files->outfile, "-", 2)) {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        files->ofp = stdout;
    } else {
        files->ofp = fopen(files->outfile, "wb");
        if(!files->ofp) {
            fprintf(stderr, "error opening output file: %s\n", files->outfile);
            return 1;
        }
    }
    return 0;
}

static void
filelist_cleanup(CommandOptions *opts)
{
    int i;

    if(opts->filelist) {
        for(i=0; i<opts->input_count; i++) {
            if(opts->filelist[i].outfile) free(opts->filelist[i].outfile);
        }
        free(opts->filelist);
    }
}

int
main(int argc, char **argv)
{
    CommandOptions opts;
    int i, err;

    memset(&opts, 0, sizeof(CommandOptions));
    err = parse_commandline(argc, argv, &opts);
    if(!opts.quiet) {
        fprintf(stderr, "\nFlake: FLAC audio encoder\n"
                        "Version %s\n"
                        "(c) 2006-2007 Justin Ruggles\n\n",
                        flake_get_version());
    }
    if(err == 2) {
        print_help(stdout);
        filelist_cleanup(&opts);
        return 0;
    } else if(err) {
        print_usage(stderr);
        filelist_cleanup(&opts);
        return 1;
    }

    for(i=0; i<opts.input_count; i++) {
        if(open_files(&opts.filelist[i])) {
            err = 1;
            break;
        }
        err = encode_file(&opts, &opts.filelist[i], (i==0));
        fclose(opts.filelist[i].ofp);
        fclose(opts.filelist[i].ifp);
        if(err) break;
    }

    filelist_cleanup(&opts);

    return err;
}
