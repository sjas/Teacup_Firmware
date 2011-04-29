
/** \file Coordinating reading and writing of SD cards.
*/

#ifndef _SD_H
#define _SD_H

#include "config_wrapper.h"

#ifdef SD_CARD_SELECT_PIN
#define SD

#include "pff.h"

#define SDFLAG_MOUNTED        0x01
#define SDFLAG_GET_FILENAME   0x02
#define SDFLAG_FILE_SELECTED  0x04
#define SDFLAG_READING        0x10
#define SDFLAG_WRITING        0x20


extern uint8_t sdflags;

extern uint8_t sdbuffer[32];
extern uint8_t sdbufptr;
extern FATFS sdfile;


void sd_init(void);

#endif /* SD_CARD_SELECT_PIN */

#endif /* _SD_H */
