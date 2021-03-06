#include <fenv.h>
#include <errno.h>
#include <math.h>
#include <string.h>

#include "dadafits_internal.h"

/**
 * Pack series of 8-bit StokesI to 1-bit
 *
 *   @param {uint[]}  downsampled[NCHANNELS_LOW * NTIMES_LOW]
 *   @param {uchar[]} packed[NCHANNELS_LOW * NTIMES_LOW / 8]
 */
void pack_sc34(unsigned int downsampled[NCHANNELS_LOW * NTIMES_LOW], unsigned char packed[NCHANNELS_LOW * NTIMES_LOW/8]) {
  unsigned int *temp1;
  unsigned char *temp2;

  // DEBUG NaNs:
  errno = 0;
  feclearexcept(FE_ALL_EXCEPT);

  int dc;
  for (dc = 0; dc < NCHANNELS_LOW; dc++) {

    // First pass: calculate average(=offset) and stdev(=scale)
    temp1 = &downsampled[dc * NTIMES_LOW];

    // Sum:
    // total sum is over 12500 samples times 4 frequencies is 50,000 samples (50,000 for sc3)
    // maxium value is 50000 * 255 = 2,750,000
    // an unsigned int should hold 4,294,967,295, so no overflow here
    //
    // Sos:
    // maxium value of a downsampled sample is 255 * 50 * 4 = 51,000
    // of these, we are summing 500 squares, so maximum value is
    //             51000 * 51000 * 500 = 1,300,500,000,000
    // unsigned long long holds 18,446,744,073,709,551,615, so ok.
    //
    unsigned int sum = 0;
    unsigned long long sos = 0;

    int dt;
    for (dt=0; dt < NTIMES_LOW; dt++) {
      sos += (*temp1) * (*temp1);
      sum += *temp1++;
    }
    float avg = sum / (1.0 * NTIMES_LOW);
    float std = sqrtf(sos / (1.0 * NTIMES_LOW) - avg * avg);

    // Second pass: convert to 1 bit
    // 0: below average, represented by nummerical value avg-std
    // 1: above average, represented by nummerical value avg+std
    // Take care of high-to-low frequency order in packed array
    fits_offset[NCHANNELS_LOW-dc] = avg - std;
    fits_scale[NCHANNELS_LOW-dc]  = 2.0 * std;

    unsigned int cutoff = avg;

    temp1 = &downsampled[dc * NTIMES_LOW];
    for (dt=0; dt < NTIMES_LOW; dt++) {
      *temp1 = *temp1 > cutoff ? 1 : 0;
      temp1++;
    }
  }

  // Third pass: pack bits in bytes, transpose to time-frequency order, order frequencies from high to low
  // packing requires NCHANNELS_LOW is divisible by 8
  int dt;
  for (dt=0; dt < NTIMES_LOW; dt++) {
    for (dc=0; dc < NCHANNELS_LOW; dc+=8) {
      // position in (transposed) packed array 
      temp2 = &packed[(dt * NCHANNELS_LOW + (dc)) / 8];
      // start point in downsampled array
      // this array is transposed and has frequencies high-to-low
      temp1 = &downsampled[(NCHANNELS_LOW - 1 - dc) * NTIMES_LOW + dt];
      // do the packing; jump to next channel in input array after each step
      // LSB is lowest channel to comply with high->low frequency order in output
      *temp2  = *temp1 ? 1     : 0;
      temp1 += NTIMES_LOW;
      *temp2 += *temp1 ? 1 << 1: 0;
      temp1 += NTIMES_LOW;
      *temp2 += *temp1 ? 1 << 2: 0;
      temp1 += NTIMES_LOW;
      *temp2 += *temp1 ? 1 << 3: 0;
      temp1 += NTIMES_LOW;
      *temp2 += *temp1 ? 1 << 4: 0;
      temp1 += NTIMES_LOW;
      *temp2 += *temp1 ? 1 << 5: 0;
      temp1 += NTIMES_LOW;
      *temp2 += *temp1 ? 1 << 6: 0;
      temp1 += NTIMES_LOW;
      *temp2 += *temp1 ? 1 << 7: 0;
    }
  }

  // DEBUG NaNs:
  int except = fetestexcept(FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW);
  if (errno || except) {
    LOG("Error: floating point exception in packing data: errno=%i (%s), fetestexcept=%i", errno, strerror(errno), except);
    switch (except) {
      case FE_INVALID: LOG("(FE_INVALID)\n"); break;
      case FE_DIVBYZERO: LOG("(FE_DIVBYZERO)\n"); break;
      case FE_OVERFLOW: LOG("(FE_OVERFLOW)\n"); break;
      case FE_UNDERFLOW: LOG("(FE_UNDERFLOW)\n"); break;
      default: LOG("-\n");
    }
  }
}

/**
 * Deinterleave (transpose) an IQUV ring buffer page to the ordering needed for FITS files
 * Note that this is probably a slow function, and is not meant to be run real-time
 * Suggested use is:
 *   1. realtime: ringbuffer -> [trigger] -> dada_dbdisk
 *   2. offline: dada_dbdisk -> ringbuffer -> dadafits
 *
 *  @param {const uchar[]} page                 Ringbuffer page with interleaved data
 *  @param {int}           ntimes               Number of time samples per page
 *  @param {int}           ntabs                Number of tabs
 *  @param {int}           sequence_length      Number of packets per
 *  @param {uchar[]}       transposed           Output buffer to hold deinterleaved data. Size: ntabs*NCHANNELS*NPOLS*ntimes
 */
void deinterleave (const unsigned char *page, const int ntimes, const int ntabs, const int sequence_length, unsigned char *transposed) {
  // ring buffer page contains matrix:
  //   [tab][channel_offset][sequence_number][8000]
  //
  // tab             : ranges from 0 to (1 or 12) mode TAB / IAB
  // channel_offset  : ranges from 0 to NCHANNELS/4 (=1536/4=384)
  // sequence_number : ranges from 0 to 25 or 25 (sc3 or sc4)
  //
  // the 8000 bytes are the original packets, containing stokes IQUV:
  //   [t0 .. t499][c0 .. c3][the 4 components IQUV]
  //
  //   t0, .., t499 = sequence_number * 500 + tx
  //   c0, c1, c2, c3 = curr_channel + 0, 1, 2, 3
  //
  // Transposed buffer will contain:
  // (NTAB,NTIME,NPOL,NCHAN) = (NTABS,12500,4,1536)
  // NOTE: data must be written in time-frequency order even though fits header
  // uses (NBIN,NFREQ,NPOL,NTIME) notation (i.e. frequency-time order)
  // additionally, frequency channels must be ordered from high to low,
  // as indicated by the negative bandwidth in the header
  // lastly, polarisations must be writen as IQUV, but pages contain VUQI

  // Tranpose by linearly processing original packets from the page
  const unsigned char *packet = page;

  // and find the matching address in the transposed buffer
  int tab = 0;
  for (tab = 0; tab < ntabs; tab++) {
    int channel_offset = 0;
    for (channel_offset = 0; channel_offset < NCHANNELS; channel_offset+=4) {
      int sequence_number = 0;
      for (sequence_number = 0; sequence_number < sequence_length; sequence_number++) {
        // process packet
        int tn,cn,pn;
        for (tn = 0; tn < 500; tn++) { // 500 samples per packet
          for (cn = 0; cn < 4; cn++) { // 4 channels per packet
            for (pn = 0; pn < NPOLS; pn++) {
              transposed[
              (tab * ntimes + 
               sequence_number * 500 + tn) * NPOLS * NCHANNELS + 
              (NPOLS - 1 - pn) * NCHANNELS +
              NCHANNELS - 1 - (channel_offset + cn)
              ] = *packet++;
            }
          }
        }
      }
    }
  }
}
