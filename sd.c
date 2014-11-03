
/** \file
  \brief Handling an SD Card.

  This is basic read and write, only. Filesystem stuff is in pff.c. Connection
  of filesystem and card r/w happens in pff_diskio.c.

  Code originally copied from http://www.arduino.cc/playground/Code/SDCARD,
  posted by Didier Longueville. Updates there are unlikely, so we can edit this
  file to our liking.

  The typical procedure of writing an SD Card is (explanation by Didier
  Longueville):

    1. Initialize SPI (done in mendel.c).
    2. Initialize SD Card.
    3. Blank vector of data (vBlock).
    4. Record data in vector of data.
    5. Copy data from vector to card.
    6. Go to 3.

  Useful links:
    http://elm-chan.org/docs/mmc/mmc_e.html
    http://www.retroleum.co.uk/mmc_cards.html
    http://www.maxim-ic.com/appnotes.cfm/an_pk/3969
*/

#include "config_wrapper.h"

#ifdef SD_CARD_SELECT_PIN

#include "delay.h"

/* Markus: As fetched from arduino.cc this code didn't compile at all. Unknown
           types, missing declarations, to name the worst. An sd.h is also
           missing.

           Functions should be renamed to sd_...() or this file should be
           renamed to sdc.c.

           For now all functions made static, so the compiler warns us about
           unused ones. Function order and code usage needs further review.
           And most importantly: calling this from pff_diskio.c.
*/
static uint8_t spi_cmd(uint8_t data);
static void spi_initialize(void);
static uint8_t sdc_cmd(uint8_t commandIndex, uint32_t arg);
static uint8_t sdc_initialize(void);
static void sdc_clearVector(void);
static uint32_t sdc_totalNbrBlocks(void);
static void sdc_readRegister(uint8_t sentCommand);
static void sdc_writeBlock(uint32_t blockIndex);
static void sdc_readBlock(uint32_t blockIndex);


/********************** SPI SECTION BELOW **********************/

// SPI Variables
uint8_t spi_err; // SPI timeout flag, must be cleared manually

// send an SPI command, includes time out management
// returns spi_err: "0" is "no error"
static uint8_t spi_cmd(uint8_t data) {
  uint8_t i;

  spi_err = 0; // reset spi error
  SPDR = data; // start the transmission by loading the output byte into the spi data register
  i = 0;
  while ( ! (SPSR & (1 << SPIF))) {
    i++;
    if (i == 0 /* wrapped around */) {
      spi_err = 1;
      return 0x00;
    }
  }
  // returned value
  return SPDR;
}

// initialize SPI port
static void spi_initialize(void) {
  #pragma GCC diagnostic ignored "-Wunused-but-set-variable"
  volatile uint8_t trash;
  #pragma GCC diagnostic pop

  SPCR = (1 << SPE) | (1 << MSTR); // spi enabled, master mode
  trash = SPSR; // dummy read registers to clear previous results
  trash = SPDR;
}

/********************** SD CARD SECTION BELOW **********************/

// SD Card variables
#define blockSize 512          // block size (default 512 bytes)
uint8_t vBlock[blockSize];     // set vector containing data that will be recorded on SD Card
uint8_t vBuffer[16];

#define GO_IDLE_STATE 0x00     // resets the SD card
#define SEND_CSD 0x09          // sends card-specific data
#define SEND_CID 0x0A          // sends card identification
#define READ_SINGLE_BLOCK 0x11 // reads a block at byte address
#define WRITE_BLOCK 0x18       // writes a block at byte address
#define SEND_OP_COND 0x29      // starts card initialization
#define APP_CMD 0x37           // prefix for application command


// Send a SD command, num is the actual index, NOT OR'ed with 0x40.
// arg is all four bytes of the argument
static uint8_t sdc_cmd(uint8_t commandIndex, uint32_t arg) {
  uint8_t i;

  WRITE(SD_CARD_SELECT_PIN, 1);   // Assert chip select for the card
  spi_cmd(0xFF);             // dummy byte
  commandIndex |= 0x40;      // command token OR'ed with 0x40
  spi_cmd(commandIndex);     // send command
  for (i = 3; i >= 0; i--) {
    spi_cmd(arg >> (i * 8)); // send argument in little endian form (MSB first)
  }
  spi_cmd(0x95);             // checksum valid for GO_IDLE_STATE, not needed
                             // thereafter, so we can hardcode this value
  spi_cmd(0xFF);             // dummy byte gives card time to process

  return spi_cmd(0xFF);      // query return value from card
}

// initialize SD card
// retuns 1 if successful
static uint8_t sdc_initialize(void) {
  uint8_t retries = 0;
  uint8_t i;

  // set slow clock: 1/128 base frequency (125Khz in this case)
  SPCR |=  (1 << SPR1) | (1 << SPR0); // set slow clock: 1/128 base frequency (125Khz in this case)
  SPSR &= ~(1 << SPI2X);            // No doubled clock frequency
  // wake up SD card
  WRITE(SD_CARD_SELECT_PIN, 0);          // deasserts card for warmup
  WRITE(MOSI, 0);
  for (i = 0; i < 10; i++) {
    spi_cmd(0xFF);                // send 10 times 8 pulses for a warmup (74 minimum)
  }
  // set idle mode
  WRITE(SD_CARD_SELECT_PIN, 1);          // assert chip select for the card
  while(sdc_cmd(GO_IDLE_STATE, 0) != 0x01) { // while SD card is not in idle state
    retries++;
    if (retries >= 0xFF) {
      return 0x00; // timed out!
    }
    delay_ms(5);
  }
  // at this stage, the card is in idle mode and ready for start up
  retries = 0;
  sdc_cmd(APP_CMD, 0); // startup sequence for SD cards 55/41
  while (sdc_cmd(SEND_OP_COND, 0) != 0x00) {
    retries++;
    if (retries >= 0xFF) {
      return 0x00; // timed out!
    }
    sdc_cmd(APP_CMD, 0);
  }
  // set fast clock, 1/4 CPU clock frequency (4Mhz in this case)
  SPCR &= ~((1 << SPR1) | (1 << SPR0)); // Clock Frequency: f_OSC / 4
  SPSR |=  (1 << SPI2X);              // Doubled Clock Frequency: f_OSC / 2

  return 0x01; // returned value (success)
}

// clear block content
static void sdc_clearVector(void) {
  uint16_t i;

  for (i = 0; i < blockSize; i++) {
    vBlock[i] = 0;
  }
}

// get nbr of blocks on SD memory card from
static uint32_t sdc_totalNbrBlocks(void) {
  uint32_t C_Size, C_Mult;

  sdc_readRegister(SEND_CSD);
  // compute size
  C_Size = ((vBuffer[0x08] & 0xC0) >> 6) | ((vBuffer[0x07] & 0xFF) << 2) |
           ((vBuffer[0x06] & 0x03) << 10);
  C_Mult = ((vBuffer[0x08] & 0x80) >> 7) | ((vBuffer[0x08] & 0x03) << 2);

  return ((C_Size + 1) << (C_Mult + 2));
}

// read SD card register content and store it in vBuffer
static void sdc_readRegister(uint8_t sentCommand) {
  uint8_t retries = 0x00;
  uint8_t res;
  uint8_t i;

  res = sdc_cmd(sentCommand, 0);
  while (res != 0x00) {
    delay_ms(1);
    retries++;
    if (retries >= 0xFF)
      return; // timed out!
    res = spi_cmd(0xFF); // retry
  }
  // wait for data token
  while (spi_cmd(0xFF) != 0xFE);
  // read data
  for (i = 0; i < 16; i++) {
    vBuffer[i] = spi_cmd(0xFF);
  }
  // read CRC (lost results in blue sky)
  spi_cmd(0xFF); // LSB
  spi_cmd(0xFF); // MSB
}

// write block on SD card
// addr is the address in bytes (multiples of block size)
static void sdc_writeBlock(uint32_t blockIndex) {
  uint16_t i;
  uint8_t retries = 0;

  while (sdc_cmd(WRITE_BLOCK, blockIndex * blockSize) != 0x00) {
    delay_ms(1);
    retries++;
    if (retries >= 0xFF)
      return; // timed out!
  }
  spi_cmd(0xFF); // dummy byte (at least one)
  // send data packet (includes data token, data block and CRC)
  // data token
  spi_cmd(0xFE);
  // copy block data
  for (i = 0; i < blockSize; i++) {
    spi_cmd(vBlock[i]);
  }
  // write CRC (lost results in blue sky)
  spi_cmd(0xFF); // LSB
  spi_cmd(0xFF); // MSB
  // wait until write is finished
  while (spi_cmd(0xFF) != 0xFF)
    delay_ms(1); // kind of NOP
}

// read block on SD card and copy data in block vector
// returns 1 if successful
static void sdc_readBlock(uint32_t blockIndex) {
  uint8_t retries = 0x00;
  uint8_t res = sdc_cmd(READ_SINGLE_BLOCK,  (blockIndex * blockSize));

  while (res != 0x00) {
    delay_ms(1);
    retries++;
    if (retries >= 0xFF)
      return; // timed out!
    res = spi_cmd(0xFF); // retry
  }
  // read data packet (includes data token, data block and CRC)
  // read data token
  while (spi_cmd(0xFF) != 0xFE) ;
  // read data block
  for (int i = 0; i < blockSize; i++) {
    vBlock[i] = spi_cmd(0xFF); // read data
  }
  // read CRC (lost results in blue sky)
  spi_cmd(0xFF); // LSB
  spi_cmd(0xFF); // MSB
}

#endif /* SD_CARD_SELECT_PIN */
