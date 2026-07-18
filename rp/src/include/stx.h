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

// Per-sector conversion risk tier (also the preflight color).
typedef enum {
  STX_TIER_LOSSLESS = 0,  // green: standard 512B sector, byte-perfect
  STX_TIER_METADATA,      // yellow: data recovered, only protection/timing
                          // metadata lost (bit-width, deleted-DAM, timing)
  STX_TIER_DATALOSS       // red: real data loss (fuzzy/CRC/RNF/non-512/missing)
} stx_tier_t;

// Overall verdict = worst tier present across the whole disk.
typedef enum {
  STX_VERDICT_LOSSLESS = 0,   // all sectors lossless (green)
  STX_VERDICT_ACCEPTABLE,     // some metadata loss, no data loss (yellow)
  STX_VERDICT_CAREFUL         // at least one data-loss sector (red)
} stx_verdict_t;

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
 * Read the 512 bytes of logical sector (cyl, side, sector_number) into buf
 * (STX_SECTOR_SIZE bytes) and report its risk tier. The best-available data
 * bytes are extracted whenever a 512-byte data block exists (lossless,
 * metadata, and recoverable data-loss cases); only sectors with no usable
 * 512-byte block (RNF / wrong size / missing) are zero-filled.
 */
stx_err_t stx_read_sector(uint8_t cyl, uint8_t side, uint8_t sector_number,
                          uint8_t *buf, stx_tier_t *tier);

/** Close the image and release the file handle. */
void stx_close(void);

const char *stx_err_str(stx_err_t err);

// --------------------------------------------------------------------------
// Preflight: scan the STX descriptors (no sector-data reads) and predict the
// conversion outcome so the UI can warn before committing.
// --------------------------------------------------------------------------

#define STX_PREFLIGHT_EXAMPLES 6

typedef struct {
  stx_verdict_t verdict;
  stx_geometry_t geom;
  int total_sectors;
  int lossless;   // green count
  int metadata;   // yellow count
  int dataloss;   // red count
  int example_count;
  struct {
    uint8_t cyl;
    uint8_t side;
    uint8_t sector;
    uint8_t tier;  // stx_tier_t of this example (metadata/dataloss only)
  } examples[STX_PREFLIGHT_EXAMPLES];
} stx_preflight_t;

/**
 * @brief Open @p path, classify every logical sector from the descriptors,
 * and fill @p out with the verdict and per-tier counts. Opens and closes the
 * image itself (does not disturb a running job). Fast: reads descriptors only.
 */
stx_err_t stx_preflight(const char *path, stx_preflight_t *out);

const char *stx_verdict_str(stx_verdict_t v);

// --------------------------------------------------------------------------
// Background STX->ST conversion job (main-loop driven, cancellable).
// One conversion at a time; converts one track (all its sectors) per poll.
// --------------------------------------------------------------------------

#define STX_PATH_MAX 448

typedef enum {
  STX_JOB_IDLE = 0,
  STX_JOB_CONVERTING,
  STX_JOB_COMPLETED,
  STX_JOB_FAILED,
  STX_JOB_CANCELLED
} stx_job_status_t;

typedef struct {
  stx_job_status_t status;
  char src[STX_PATH_MAX];
  char dest[STX_PATH_MAX];  // output .st path
  stx_geometry_t geom;
  int tracks_total;
  int tracks_done;
  int sectors_lossless;    // green
  int sectors_metadata;    // yellow (data recovered, metadata lost)
  int sectors_dataloss;    // red (real data loss)
  int incomplete_tracks;   // tracks with >=1 data-loss sector
  bool cancel_requested;
  stx_err_t last_error;
} stx_job_info_t;

/**
 * @brief Start converting @p stx_path to a `.st` in @p dest_folder (named
 * after the source with the extension replaced). Returns false on immediate
 * failure (bad geometry, cannot open); the info then carries the reason.
 */
bool stx_job_start(const char *stx_path, const char *dest_folder);

/** Advance the conversion by one track. Call while stx_job_is_active(). */
void stx_job_poll(void);

/** Request cancellation; stops at the next poll and removes the partial file. */
void stx_job_cancel(void);

/** True while a conversion is in progress. */
bool stx_job_is_active(void);

/** Snapshot the current job state. */
void stx_job_get_info(stx_job_info_t *out);

/** Human-readable string for a job status. */
const char *stx_job_status_str(stx_job_status_t status);

#endif  // STX_H
