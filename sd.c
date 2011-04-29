
/** \file Coordinating reading and writing of SD cards.
*/

#include "sd.h"

#ifdef SD

// See sd.h for flag values.
uint8_t sdflags;

uint8_t sdbuffer[32];
uint8_t sdbufptr;
FATFS sdfile;


void sd_init(void) {
  sdflags = 0;

  /**
    This mounts an SD card, which is already inserted at startup, immediately.
    Not sure wether this is actually useful. It might be more useful to try
    this every second in clock.c to detect inserted cards immediately.
  */
  if (pf_mount(&sdfile) == FR_OK)
    sdflags = SDFLAG_MOUNTED;
}

#endif /* SD */
