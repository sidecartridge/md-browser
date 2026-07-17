/**
 * File: stx.h
 * Author: Diego Parrilla Santamaría
 * Date: July 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Read Atari ST .STX (Pasti) floppy images and extract the
 * standard logical sectors. Lossy by design: copy protection, weak/fuzzy
 * bits, deleted/non-standard sectors and timing tricks cannot be represented
 * in a raw .ST and are lost (see D-06). FatFs-backed, one archive at a time.
 */

#ifndef STX_H
#define STX_H

#include <stdbool.h>
#include <stdint.h>

#define STX_SECTOR_SIZE 512
#define STX_MAX_TRACKS 168  // 84 cylinders * 2 sides, generous upper bound

typedef enum {
  STX_OK = 0,
  STX_ERR_OPEN,     // cannot open the .stx on the SD card
  STX_ERR_FORMAT,   // not a valid Pasti/STX file
  STX_ERR_READ,     // SD read error
  STX_ERR_STATE     // no image open / API misuse
} stx_err_t;

// Result of reading one logical sector.
typedef enum {
  STX_SECTOR_STANDARD = 0,  // clean 512-byte standard sector, data valid
  STX_SECTOR_MISSING,       // no such sector on the track (zero-filled)
  STX_SECTOR_NONSTANDARD    // present but protected/non-standard (zero-filled)
} stx_sector_result_t;

typedef struct {
  uint8_t sides;         // 1 or 2
  uint8_t cylinders;     // e.g. 80/82/84
  uint8_t sectors;       // sectors per track (e.g. 9/10/11)
  bool standard_geometry;  // false if geometry couldn't be determined cleanly
} stx_geometry_t;

/** Open a .STX and index its track records. One image open at a time. */
stx_err_t stx_open(const char *path);

/** Best-effort geometry from the standard tracks. */
stx_err_t stx_get_geometry(stx_geometry_t *out);

/**
 * Read the 512 bytes of logical sector (cyl, side, sector_number) into buf.
 * buf must be STX_SECTOR_SIZE bytes. On MISSING/NONSTANDARD the buffer is
 * zero-filled and the corresponding result is returned (still STX_OK rc).
 */
stx_err_t stx_read_sector(uint8_t cyl, uint8_t side, uint8_t sector_number,
                          uint8_t *buf, stx_sector_result_t *result);

/** Close the image and release the file handle. */
void stx_close(void);

const char *stx_err_str(stx_err_t err);

#endif  // STX_H
