/**
 * program: dadafits
 *          Written for the AA-Alert project, ASTRON
 *
 * Purpose: connect to a ring buffer and create FITS output per TAB
 *          Depending on science case and mode, reduce time and frequency resolution to 1 bit
 *          Fits files are created using templates
 *
 * Science case 3, mode 0:
 *          template: sc34_1bit_I_reduced.txt
 *
 *          A ringbuffer page is interpreted as an array of Stokes I:
 *          [NTABS, NCHANNELS, padded_size] = [12, 1536, > 12500]
 *
 *          The code reduces (by summation) from 12500 to 500 timesteps
 *          and from 1536 to 384 channels.
 *          Time dimension padding is required by other programes (GPU pipeline)
 *          that connects to the same ringbuffer.
 *
 * Science case 3, mode 1:
 *          template: sc3_8bit_IQUV_full.txt
 *
 *          A ringbuffer page is interpreted as an interleaved array of Stokes IQUV:
 *          [tab][channel_offset][sequence_number][packet]
 *
 *          tab             := ranges from 0 to NTABS
 *          channel_offset  := ranges from 0 to NCHANNELS/4 (384)
 *          sequence_number := ranges from 0 to 25
 *
 *          with a packet: [t0 .. t499][c0 .. c3][IQUV] total of 500*4*4=8000 bytes
 *          t = tn + sequence_number * 25
 *          c = cn + channel_offset * 4
 *
 * Science case 3, mode 2:
 *          template: sc34_1bit_I_reduced.txt
 *
 *          A ringbuffer page is interpreted as an array of Stokes I:
 *          [NTABS, NCHANNELS, padded_size] = [1, 1536, > 12500]
 *
 *          The code reduces (by summation) from 12500 to 500 timesteps
 *          and from 1536 to 384 channels.
 *          Time dimension padding is required by other programes (GPU pipeline)
 *          that connects to the same ringbuffer.
 *
 * Science case 4, mode 0:
 *          template: sc34_1bit_I_reduced.txt
 *
 *          A ringbuffer page is interpreted as an array of Stokes I:
 *          [NTABS, NCHANNELS, padded_size] = [12, 1536, > 25000]
 *
 *          The code reduces (by summation) from 25000 to 500 timesteps
 *          and from 1536 to 384 channels.
 *          Time dimension padding is required by other programes (GPU pipeline)
 *          that connects to the same ringbuffer.
 *
 * Science case 4, mode 1:
 *          template: sc4_8bit_IQUV_full.txt
 *
 *          A ringbuffer page is interpreted as an interleaved array of Stokes IQUV:
 *          [tab][channel_offset][sequence_number][packet]
 *
 *          tab             := ranges from 0 to NTABS
 *          channel_offset  := ranges from 0 to NCHANNELS/4 (384)
 *          sequence_number := ranges from 0 to 50
 *
 *          with a packet: [t0 .. t499][c0 .. c3][IQUV] total of 500*4*4=8000 bytes
 *          t = tn + sequence_number * 50
 *          c = cn + channel_offset * 4
 *
 * Science case 4, mode 2:
 *          template: sc34_1bit_I_reduced.txt
 *
 *          A ringbuffer page is interpreted as an array of Stokes I:
 *          [NTABS, NCHANNELS, padded_size] = [1, 1536, > 25000]
 *
 *          The code reduces (by summation) from 25000 to 500 timesteps
 *          and from 1536 to 384 channels.
 *          Time dimension padding is required by other programes (GPU pipeline)
 *          that connects to the same ringbuffer.
 *
 * Author: Jisk Attema, Netherlands eScience Center
 * Licencse: Apache v2.0
 */

#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>

#include "dada_hdu.h"
#include "ascii_header.h"

#include "dadafits_internal.h"

FILE *runlog = NULL;

const char *science_modes[] = {"I+TAB", "IQUV+TAB", "I+IAB", "IQUV+IAB"};

// Variables read from ring buffer header
float min_frequency = 1492;
float channel_bandwidth = 0.1953125;

// Variables set from commandline
int make_synthesized_beams = 0;

// Memory buffers
unsigned int downsampled[NCHANNELS_LOW * NTIMES_LOW];
unsigned char packed[NCHANNELS_LOW * NTIMES_LOW / 8];
unsigned char *transposed = NULL; // Stokes IQUV buffer of approx 2 GB, allocated only when necessary
unsigned char *synthesized = NULL; // Stokes IQUV for a single synthesized beam

/**
 * Open a connection to the ringbuffer
 *
 * @param {char *} key String containing the shared memory key as hexadecimal number
 * @returns {hdu *} A connected HDU
 */
dada_hdu_t *init_ringbuffer(char *key) {
  uint64_t nbufs;

  multilog_t* multilog = NULL; // TODO: See if this is used in anyway by dada

  // create hdu
  dada_hdu_t *hdu = dada_hdu_create (multilog);

  // init key
  key_t shmkey;
  sscanf(key, "%x", &shmkey);
  dada_hdu_set_key(hdu, shmkey);
  LOG("dadafits SHMKEY: %s\n", key);

  // connect
  if (dada_hdu_connect (hdu) < 0) {
    LOG("ERROR in dada_hdu_connect\n");
    exit(EXIT_FAILURE);
  }

  // Make data buffers readable
  if (dada_hdu_lock_read(hdu) < 0) {
    LOG("ERROR in dada_hdu_open_view\n");
    exit(EXIT_FAILURE);
  }

  // get write address
  char *header;
  uint64_t bufsz;
  LOG("dadafits reading header");
  header = ipcbuf_get_next_read (hdu->header_block, &bufsz);
  if (! header || ! bufsz) {
    LOG("ERROR. Get next header block error\n");
    exit(EXIT_FAILURE);
  }

  // parse header
  unsigned int uintValue;
  float floatValue[2];
  ascii_header_get(header, "MIN_FREQUENCY", "%f", &min_frequency);
  ascii_header_get(header, "BW", "%f", &channel_bandwidth);

  // tell the ringbuffer the header has been read
  if (ipcbuf_mark_cleared(hdu->header_block) < 0) {
    LOG("ERROR. Cannot mark the header as cleared\n");
    exit(EXIT_FAILURE);
  }

  LOG("psrdada HEADER:\n%s\n", header);

  return hdu;
}

/**
 * Print commandline options
 */
void printOptions() {
  printf("usage: dadafits -k <hexadecimal key> -l <logfile> -c <science_case> -m <science_mode> -b <padded_size> -t <template> -d <output_directory> -S <synthesized beam table> -s <synthesize these beams>\n");
  printf("e.g. dadafits -k dada -l log.txt -c 3 -m 0 -b 25088 -t /full/path/template.txt -S table.txt -s 0,1,4-8 -d /output/directory\n");
  return;
}

/**
 * Parse commandline
 */
void parseOptions(int argc, char *argv[], char **key, int *padded_size, char **logfile, int *science_case, int *science_mode, char **template_file, char **table_name, char **sb_selection, char **output_directory) {
  int c;

  int setk=0, setb=0, setl=0, setc=0, setm=0, sett=0, setd=0;
  while((c=getopt(argc,argv,"k:b:l:c:m:t:d:s:S:"))!=-1) {
    switch(c) {
      // -t <template_file>
      case('t'):
        *template_file = strdup(optarg);
        sett=1;
        break;

      // OPTIONAL: -d <output_directory>
      // DEFAULT: CWD
      case('d'):
        *output_directory = strdup(optarg);
        setd=1;
        break;

      // -k <hexadecimal_key>
      case('k'):
        *key = strdup(optarg);
        setk=1;
        break;

      // -b padded_size (bytes)
      case('b'):
        *padded_size = atoi(optarg);
        setb=1;
        break;

      // -l log file
      case('l'):
        *logfile = strdup(optarg);
        setl=1;
        break;

      // -c science_case
      case('c'):
        *science_case = atoi(optarg);
        setc = 1;
        break;

      // -m science_mode
      case('m'):
        *science_mode = atoi(optarg);
        setm = 1;
        break;

      // OPTIONAL: -S synthesized beam table
      case('S'):
        *table_name = strdup(optarg);
        break;

      // OPTIONAL: -s synthesized beam selection
      case('s'):
        *sb_selection = strdup(optarg);
        break;

      default:
        printOptions();
        exit(0);
    }
  }

  // Required arguments
  if (!setk || !setl || !setb || !setc || !setm || !sett) {
    printOptions();
    exit(EXIT_FAILURE);
  }
}

int main (int argc, char *argv[]) {
  char *key;
  int padded_size;
  char *logfile;
  char *template_file;
  char *table_name = NULL; // optional argument
  char *sb_selection = NULL; // optional argument, defaults to all beams
  char *output_directory = NULL; // defaults to CWD
  int science_case;
  int science_mode;
  int ntabs;
  int nchannels; // for FITS outputfile (so after optional compression)
  int ntimes; // for FITS outputfile (so after optional compression)
  int npols; // for FITS outputfile (so after optional compression)
  int sequence_length;

  // parse commandline
  parseOptions(argc, argv, &key, &padded_size, &logfile, &science_case, &science_mode, &template_file, &table_name, &sb_selection, &output_directory);

  // set up logging
  if (logfile) {
    runlog = fopen(logfile, "w");
    if (! runlog) {
      LOG("ERROR opening logfile: %s\n", logfile);
      exit(EXIT_FAILURE);
    }
    LOG("Logging to logfile: %s\n", logfile);
    free (logfile);
  }

  LOG("dadafits version: " VERSION "\n");

  if (table_name) {
    LOG("Writing synthesized beams\n");
    make_synthesized_beams = 1;
    read_synthesized_beam_table(table_name);
    parse_synthesized_beam_selection(sb_selection);
  } else {
    LOG("Writing TABs (not synthesized beams)\n");
    make_synthesized_beams = 0;
  }

  switch (science_mode) {
    case 0: // I + TAB to be compressed and downsampled
      ntimes = NTIMES_LOW;
      nchannels = NCHANNELS_LOW;
      ntabs = 12;
      npols = 1;
      if (make_synthesized_beams) {
        LOG("Cannot write synthesized beams for compressed I+TAB\n");
        exit(EXIT_FAILURE);
      }
      break;
    case 1: // IQUV + TAB to deinterleave
      ntimes = science_case == 3 ? 12500 : 25000;
      nchannels = NCHANNELS;
      ntabs = 12;
      npols = 4;
      break;
    case 2: // I + IAB to be compressed and downsampled
      ntimes = NTIMES_LOW;
      nchannels = NCHANNELS_LOW;
      ntabs = 1;
      npols = 1;
      if (make_synthesized_beams) {
        LOG("Cannot write synthesized beams for compressed I+IAB\n");
        exit(EXIT_FAILURE);
      }
      break;
    case 3: // IQUV + IAB to deinterleave
      ntimes = science_case == 3 ? 12500 : 25000;
      nchannels = NCHANNELS;
      ntabs = 1;
      npols = 4;
      break;
    default:
      LOG("Illegal science mode %i\n", science_mode);
      exit(EXIT_FAILURE);
  }

  switch (science_case) {
    case 3:
      sequence_length = 25;
      if (padded_size < 12500) {
        LOG("Error: padded_size too small, should be at least 12500 for science case 3\n");
        exit(EXIT_FAILURE);
      }
      break;
    case 4:
      sequence_length = 50;
      if (padded_size < 25000) {
        LOG("Error: padded_size too small, should be at least 25000 for science case 4\n");
        exit(EXIT_FAILURE);
      }
      break;
    default:
      LOG("Illegal science case %i\n", science_case);
      exit(EXIT_FAILURE);
  }

  LOG("Science mode: %i [ %s ]\n", science_mode, science_modes[science_mode]);
  LOG("Science case: %i\n", science_case);
  LOG("Output to FITS tabs: %i, channels: %i, polarizations: %i, samples: %i\n", ntabs, nchannels, npols, ntimes);

  if (science_mode == 1 || science_mode == 3) {
    LOG("Allocating Stokes IQUV transpose buffer (%i,%i,%i,%i)\n", ntabs, NCHANNELS, NPOLS, ntimes);
    transposed = malloc(ntabs * NCHANNELS * NPOLS * ntimes * sizeof(char));
    if (transposed == NULL) {
      LOG("Could not allocate stokes IQUV transpose matrix\n");
      exit(EXIT_FAILURE);
    }
  }

  if (make_synthesized_beams) {
    LOG("Allocating Stokes IQUV synthesized beam buffer (1,%i,%i,%i)\n", NCHANNELS, NPOLS, ntimes);
    synthesized = malloc(1 * NCHANNELS * NPOLS * ntimes * sizeof(char));
    if (synthesized == NULL) {
      LOG("Could not allocate stokes IQUV synthesized beam buffer\n");
      exit(EXIT_FAILURE);
    }
  }

  int quit = 0;
  long page_count = 0;

  // Trap Ctr-C to properly close fits files on exit
  signal(SIGTERM, fits_error_and_exit);

#ifdef DRY_RUN
  // do 10 iterations with random data, ignore ringbuffer
  int mysize = ntabs * NCHANNELS * npols * padded_size;
  LOG("DRY RUN FAKE DATA: ntabs=%i, nchannels=%i, npols=%i, padded_size=%i, mysize=%i\n", ntabs, NCHANNELS, npols, padded_size, mysize);
  char *page = malloc(mysize);

  dadafits_fits_init(template_file, output_directory, ntabs, make_synthesized_beams, min_frequency, channel_bandwidth * NCHANNELS / nchannels);

  int g_seed = 1234;
  inline unsigned char fastrand() {
    g_seed = (214013*g_seed+2531011);
    // return (g_seed>>16)&0x7FFF;
    return (g_seed>>16)&0xFF;
  }

  while(page_count < 10) {
    int kkk;
    for(kkk=0; kkk<mysize; kkk++) {
      page[kkk] = fastrand();
    }
#else
  // normal operation with ringbuffer
  char *page = NULL;

  // must init ringbuffer before fits, as this reads parameters
  // like channel_bandwidth from ring buffer header
  dada_hdu_t *ringbuffer = init_ringbuffer(key);
  ipcbuf_t *data_block = (ipcbuf_t *) ringbuffer->data_block;
  ipcio_t *ipc = ringbuffer->data_block;
  uint64_t bufsz = ipc->curbufsz;

  dadafits_fits_init(template_file, output_directory, ntabs, make_synthesized_beams, channel_bandwidth * NCHANNELS / nchannels);

  while(!quit && !ipcbuf_eod(data_block)) {
    page = ipcbuf_get_next_read(data_block, &bufsz);
#endif

    int tab; // Tied array beam
    int sb;  // synthesized beam

    if (! page) {
      quit = 1;
    } else {
      switch (science_mode) {
        // stokesI data to compress, downsample, and write
        case 0:
        case 2:
          for (tab = 0; tab < ntabs; tab++) {
            // move data from the page to the downsampled array
            if (science_case == 3) {
              downsample_sc3(&page[tab * NCHANNELS * padded_size], padded_size);
            } else if (science_case == 4) {
              downsample_sc4(&page[tab * NCHANNELS * padded_size], padded_size);
            } else {
              exit(EXIT_FAILURE);
            }
            // pack data from the downsampled array to the packed array,
            // and set scale and offset arrays with used values
            pack_sc34();

            // NOTE: Use hardcoded values instead of the variables ntimes, nchannels, npols
            // because at this point in the program they can only have these values, and this could
            // possibly allow some more optimizations
            write_fits(
              tab,
              NCHANNELS_LOW,
              1, // only Stokes I
              page_count + 1, // page_count starts at 0, but FITS rowid at 1
              NCHANNELS_LOW * NTIMES_LOW / 8,
              packed // write data from the packed array to file, also uses scale, weights, and offset arrays
            );
          }
          break;

        // stokesIQUV data to (optionally synthesize) and write
        case 1:
        case 3:
          // transpose data from page to transposed buffer
          deinterleave(page, ntabs, sequence_length);

          if (make_synthesized_beams) {
            // synthesize beams
            //
            // Input: transposed buffer   [TABS, CHANNELS, POLS, TIMES]
            // Output: synthesized buffer [CHANNELS, POLS, TIMES]
            
            for (sb = 0; sb < synthesized_beam_count; sb++) {
              if (synthesized_beam_selected[sb]) {
                int band;

                // a subband contains 1536/32=48 frequencies from a TAB
                for (band = 0; band < NSUBBANDS; band++) {
                  // find the TAB for this subband, and check validity
                  tab = synthesized_beam_table[sb][band]; // TODO: subband from table -> tab?
                  if (tab == SUBBAND_UNSET || tab > ntabs) {
                    LOG("Error: illegal subband index %i in synthesized beam %i\n", tab, sb);
                    exit(EXIT_FAILURE);
                  }

                  memcpy(
                    &synthesized[band * FREQS_PER_SUBBAND * NPOLS * ntimes],
                    &transposed[(tab * NCHANNELS + band * FREQS_PER_SUBBAND) * NPOLS * ntimes],
                    FREQS_PER_SUBBAND * NPOLS * ntimes
                  );
                }

                // write data from synthesized buffer
                write_fits(
                  sb,
                  NCHANNELS,
                  NPOLS, // full Stokes IQUV
                  page_count + 1, // page_count starts at 0, but FITS rowid at 1
                  NCHANNELS * NPOLS * ntimes,
                  synthesized
                );
              }
            }
          } else {
            LOG("TABs %i\n", ntabs);
            // do not synthesize, but use TABs
            for (tab = 0; tab < ntabs; tab++) {
              // write data from transposed buffer, also uses scale, weights, and offset arrays (but set to neutral values)
              write_fits(
                tab,
                NCHANNELS,
                NPOLS, // full Stokes IQUV
                page_count + 1, // page_count starts at 0, but FITS rowid at 1
                NCHANNELS * NPOLS * ntimes,
                &transposed[tab * NCHANNELS * NPOLS * ntimes]
              );
            }
          }
          break;

        default:
          // should not happen
          LOG("Illegal science mode %i\n", science_mode);
          quit = 1;
          break;
      }
#ifndef DRY_RUN
      ipcbuf_mark_cleared((ipcbuf_t *) ipc);
#endif
      page_count++;
    }
  }

#ifndef DRY_RUN
  if (ipcbuf_eod(data_block)) {
    LOG("End of data received\n");
  }

  dada_hdu_unlock_read(ringbuffer);
  dada_hdu_disconnect(ringbuffer);
#endif

  LOG("Read %li pages\n", page_count);

  close_fits();
}