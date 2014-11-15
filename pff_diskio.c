/*-----------------------------------------------------------------------*/
/* Low level disk I/O module skeleton for Petit FatFs (C)ChaN, 2014      */
/*                                                                       */
/* ... completed by ...                                                  */
/*                                                                       */
/* MMCv3/SDv1/SDv2 (in SPI mode) control module  (C)ChaN, 2010           */
/* File mmc_avr.c, part of avr_complex in ffsample, a package with       */
/* sample implementations on http://elm-chan.org/fsw/ff/00index_e.html   */
/*-----------------------------------------------------------------------*/
/* Changes for Teacup:

  Nov. 2014:
  - Brought in all the funktional code from the sample code mentioned
    above.
*/

#include "sd.h"

#ifdef SD

#include <stdlib.h>

#include "pff_diskio.h"
#include "delay.h"


/* MMC card type flags (MMC_GET_TYPE) */
#define CT_MMC      0x01          /* MMC ver 3 */
#define CT_SD1      0x02          /* SD ver 1 */
#define CT_SD2      0x04          /* SD ver 2 */
#define CT_SDC      (CT_SD1|CT_SD2) /* SD */
#define CT_BLOCK    0x08          /* Block addressing */

/* Definitions for MMC/SDC command */
#define CMD0        (0)           /* GO_IDLE_STATE */
#define CMD1        (1)           /* SEND_OP_COND (MMC) */
#define ACMD41      (0x80+41)     /* SEND_OP_COND (SDC) */
#define CMD8        (8)           /* SEND_IF_COND */
#define CMD9        (9)           /* SEND_CSD */
#define CMD10       (10)          /* SEND_CID */
#define CMD12       (12)          /* STOP_TRANSMISSION */
#define ACMD13      (0x80+13)     /* SD_STATUS (SDC) */
#define CMD16       (16)          /* SET_BLOCKLEN */
#define CMD17       (17)          /* READ_SINGLE_BLOCK */
#define CMD18       (18)          /* READ_MULTIPLE_BLOCK */
#define CMD23       (23)          /* SET_BLOCK_COUNT (MMC) */
#define ACMD23      (0x80+23)     /* SET_WR_BLK_ERASE_COUNT (SDC) */
#define CMD24       (24)          /* WRITE_BLOCK */
#define CMD25       (25)          /* WRITE_MULTIPLE_BLOCK */
#define CMD32       (32)          /* ERASE_ER_BLK_START */
#define CMD33       (33)          /* ERASE_ER_BLK_END */
#define CMD38       (38)          /* ERASE */
#define CMD55       (55)          /* APP_CMD */
#define CMD58       (58)          /* READ_OCR */

/* Port controls  (Platform dependent) */
// TODO: used only once(?) and should go into the code directly.
#define MMC_WP      (PINB & 0x20) /* Write protected.
                                     yes:true, no:false, default:false */
#define FCLK_SLOW() SPCR = 0x52   /* Set slow clock (F_CPU / 64) */
#define FCLK_FAST() SPCR = 0x50   /* Set fast clock (F_CPU / 2) */


static volatile DSTATUS card_status = STA_NOINIT;  /* Disk status */

static BYTE card_type;                             /* Card type flags */


/*! Turn SD card on and off.

  In Teacup, we currently don't support card power control, so this just
  fiddles a bit with the SPI interface.

  For an implementation of real power control, see the original control
  module (comment at file start). It requires at least an I/O pin.
*/
static void power_on(void) {
  /* Turn Chip Select off. Will be enabled on command writes. */
  WRITE(SD_CARD_SELECT_PIN, 1);
  SET_OUTPUT(SD_CARD_SELECT_PIN);

  SPCR = 0x52;            /* Enable SPI function in mode 0 */
  SPSR = 0x01;            /* SPI 2x mode */
}

/*! See power_on(). */
static void power_off(void) {
  /* Turn Chip Select off. */
  WRITE(SD_CARD_SELECT_PIN, 1);
}

/*! Exchange a byte over SPI.

  That is: write one byte and read another byte at the same time. This
  is how SPI works, reading a byte without sending one or vice versa
  isn't even possible. If you want to just receive, send a dummy byte.
*/
static BYTE xchg_spi(BYTE dat) {
    SPDR = dat;
    loop_until_bit_is_set(SPSR, SPIF);
    return SPDR;
}

/*! Wait for card ready.

  \param wt timeout in milliseconds

  \return  1: Successful, 0: Timeout
*/
static uint8_t wait_ready(uint16_t wait_ms) {
  uint8_t d = 0x00;

  while (wait_ms) {
    d = xchg_spi(0xFF);
    if (d != 0xFF)
      return 1;
    delay_ms(1);
    wait_ms--;
  }

  return 0;
}

/*! Deselect the card and release SPI bus.
*/
static void deselect(void) {
  WRITE(SD_CARD_SELECT_PIN, 1);
  xchg_spi(0xFF); /* Dummy clock (force DO hi-z for multiple slave SPI) */
}

/*! Select the card and wait for ready.

  \return  1: Successful, 0: Timeout
*/
static uint8_t select(void) {
  WRITE(SD_CARD_SELECT_PIN, 0);
  xchg_spi(0xFF); /* Dummy clock (force DO enabled) */

  if (wait_ready(500))
    return 1;  /* OK */

  deselect();
  return 0;   /* Timeout */
}

/*! Send a command packet to MMC/SD Card.

  \param cmd command index
  \param arg argument

  \return R1 resp (bit7==1:Send failed)
*/
static BYTE send_cmd(BYTE cmd, DWORD arg) {
  BYTE n, res;

  if (cmd & 0x80) {   /* ACMD<n> is the command sequence of CMD55-CMD<n> */
    cmd &= 0x7F;
    res = send_cmd(CMD55, 0);
    if (res > 1)
      return res;
  }

  /* Select the card and wait for ready except to stop multiple block read. */
  if (cmd != CMD12) {
    deselect();  // TODO: really neccessary?
    if ( ! select())
      return 0xFF;
  }

  /* Send command packet */
  xchg_spi(0x40 | cmd);               /* Start + Command index */
  xchg_spi((BYTE)(arg >> 24));        /* Argument[31..24] */
  xchg_spi((BYTE)(arg >> 16));        /* Argument[23..16] */
  xchg_spi((BYTE)(arg >> 8));         /* Argument[15..8] */
  xchg_spi((BYTE)arg);                /* Argument[7..0] */
  n = 0x01;                           /* Dummy CRC + Stop */
  if (cmd == CMD0) n = 0x95;          /* Valid CRC for CMD0(0) + Stop */
  if (cmd == CMD8) n = 0x87;          /* Valid CRC for CMD8(0x1AA) Stop */
  xchg_spi(n);

  /* Receive command response */
  if (cmd == CMD12)                   /* Skip a stuff byte when stop reading. */
    xchg_spi(0xFF);
  n = 10;                             /* Wait for a response, try 10 times. */
  do {
    res = xchg_spi(0xFF);
    n--;
  } while ((res & 0x80) && n);

  return res;
}

/*! Send plain data to MMC/SD Card.

  \param buffer  Received data should go in here. If NULL, data is
                 discarded.
  \param count   Number of bytes to read. Can be zero, in which case
                 nothing happens.

  This is a plain, unverified read. Just fetch bytes. Initiating the read
  has to be done before entering here.
*/
static void read_data(uint8_t *buffer, uint16_t count) {
  while (count) {
    SPDR = 0xFF;
    loop_until_bit_is_set(SPSR, SPIF);
    if (buffer) {
      *buffer = SPDR;
      buffer++;
    }
    count--;
  }
}

/*! Initialize Disk Drive.

  Also find out which kind of card we're talking to.

  Description about what's going on here see
  http://elm-chan.org/docs/mmc/mmc_e.html, center section.
*/
DSTATUS disk_initialize(void) {
  BYTE n, cmd, ty, ocr[4];

  #ifdef SD_CARD_DETECT_PIN
    /* Original code uses an 10 ms repeating timer to set/clear STA_NODISK
       and STA_PROTECT (write protection). We use no such timer for better
       performance, at the risk that these flags are not always up to date. */
    if ( ! READ(SD_CARD_DETECT_PIN)) { /* No card in socket. */
      card_status & STA_NODISK;
      return card_status;
    }
  #endif

  power_on();
  FCLK_SLOW();
  for (n = 10; n; n--)                 /* 80 dummy clocks */
    xchg_spi(0xFF);

  /* Find card type: MMCv3, SDv1 or SDv2 */
  ty = 0;
  if (send_cmd(CMD0, 0) == 1) {        /* Software reset, enter idle state. */
    uint8_t timeout = 250;             /* 250 * 4 ms = 1000 ms */

    if (send_cmd(CMD8, 0x1AA) == 1) {  /* SDv2? (rejected on others) */
      for (n = 0; n < 4; n++)          /* Get trailing return value of R7. */
        xchg_spi(0xFF);

      /* Wait for leaving idle state (ACMD41 with HCS bit). */
      while (timeout && send_cmd(ACMD41, 1UL << 30)) {
        delay_ms(4);
        timeout--;
      }

      /* Find out wether it's a block device (CCS bit in OCR). */
      if (timeout && send_cmd(CMD58, 0) == 0) {
        for (n = 0; n < 4; n++)
          ocr[n] = xchg_spi(0xFF);
        ty = (ocr[0] & 0x40) ? CT_SD2 | CT_BLOCK : CT_SD2;  /* SDv2 */
      }
    }
    else {                          /* SDv1 or MMCv3 */
      if (send_cmd(ACMD41, 0) <= 1)   {
        ty = CT_SD1; cmd = ACMD41;  /* SDv1 */
      }
      else {
        ty = CT_MMC; cmd = CMD1;    /* MMCv3 */
      }

      /* Wait for leaving idle state. */
      while (timeout && send_cmd(cmd, 0)) {
        delay_ms(4);
        timeout--;
      }

      /* Set R/W block length to 512 */
      if ( ! timeout || send_cmd(CMD16, 512) != 0)
        ty = 0;
    }
  }
  card_type = ty;
  deselect();

  if (ty) {  /* Initialization succeded */
    card_status &= ~STA_NOINIT;  /* Clear STA_NOINIT */
    FCLK_FAST();
  }
  else {
    power_off();
  }

  return card_status;
}

/*! Read Partial Sector.

  \param buffer  Received data should go in here.
  \param sector  Sector number (LBA).
  \param offset  Offset into the sector.
  \param count   Byte count (bit15:destination).

  \return Success

  This is the main reading function. Forming this into directory listings,
  file reads and such stuff is done by Petit FatFs it's self.

  Description about what's going on here see
  http://elm-chan.org/docs/mmc/mmc_e.html, bottom section.
*/
#if _USE_READ
DRESULT disk_readp(BYTE* buffer, DWORD sector, UINT offset, UINT count) {
  BYTE token = 0xFF;
  uint8_t timeout = 100;             /* 100 * 2 ms = 200 ms */

  // TODO: remove this when SD Card reading works.
  if ( ! buffer || ! sector || ! count)
    return RES_PARERR;
  if ( ! card_type || card_status & STA_NOINIT)
    return RES_NOTRDY;

  /* Convert to byte address on non-block cards. */
  if ( ! (card_type & CT_BLOCK))
    sector *= 512;

  /* Read one sector, copy only as many bytes as required. */
  if (send_cmd(CMD17, sector) == 0) {
    /* Wait for data packet in timeout of 200 ms. */
    while (timeout && (token == 0xFF)) {
      token = xchg_spi(0xFF);
      delay_ms(2);
      timeout--;
    }
    if (token != 0xFE) {    /* No valid data token. */
      deselect();
      return RES_ERROR;
    }

    /* Discard unwanted offset */
    read_data(NULL, offset);

    /* Read wanted data. */
    read_data(buffer, count);

    /* Discard rest of the block and 2 bytes CRC. */
    read_data(NULL, (514 - offset - count));
  }
  else {
    return RES_ERROR;
  }

  deselect();               /* Every send_cmd() selects. */

  return RES_OK;
}
#endif /* _USE_READ */


/*! Write Partial Sector.

  \param buff   Pointer to the data to be written.
                NULL: Initiate/Finalize write operation
  \param sc     Sector number (LBA) or Number of bytes to send.

  \return Success.

  This is the main writing function. Forming this into file writes and such
  stuff is done by Petit FatFs it's self.

  Description about what's going on here see
  http://elm-chan.org/docs/mmc/mmc_e.html, bottom section.
*/
#if _USE_WRITE
DRESULT disk_writep(const BYTE* buff, DWORD sc) {
  #error disk_writep() not yet implemented
  DRESULT res;

  if ( ! buff) {
    if (sc) {

      // Initiate write process

    }
    else {

      // Finalize write process

    }
  }
  else {

    // Send data to the disk

  }

  return res;
}
#endif /* _USE_WRITE */

#endif /* SD */
