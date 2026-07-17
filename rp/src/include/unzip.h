/**
 * File: unzip.h
 * Author: Diego Parrilla Santamaría
 * Date: July 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Extract .zip archives from the microSD card. Thin wrapper over
 * the vendored miniz ZIP reader, backed by FatFs I/O. One archive open at a
 * time (matches the single-job model of download.c / copy.c).
 */

#ifndef UNZIP_H
#define UNZIP_H

#include <stdbool.h>
#include <stdint.h>

#define UNZIP_NAME_MAX 256

typedef enum {
  UNZIP_OK = 0,
  UNZIP_ERR_OPEN,         // cannot open the .zip on the SD card
  UNZIP_ERR_FORMAT,       // not a valid zip / central directory unreadable
  UNZIP_ERR_READ,         // SD read error
  UNZIP_ERR_WRITE,        // SD write error / out of space
  UNZIP_ERR_UNSUPPORTED,  // encrypted, or a compression method we don't support
  UNZIP_ERR_PATH,         // unsafe or oversized entry path
  UNZIP_ERR_STATE         // no archive open / API misuse
} unzip_err_t;

typedef struct {
  char name[UNZIP_NAME_MAX];  // entry name as stored (forward slashes)
  uint64_t uncomp_size;       // uncompressed size in bytes
  bool is_dir;                // directory entry
  bool supported;             // stored/deflated and not encrypted
} unzip_entry_t;

/**
 * @brief Open a .zip on the SD card for reading. Only one archive may be open
 * at a time; call unzip_close() before opening another.
 */
unzip_err_t unzip_open(const char *zip_path);

/**
 * @brief Number of entries in the open archive, or -1 if none is open.
 */
int unzip_num_entries(void);

/**
 * @brief Fill @p out with metadata for entry @p index.
 */
unzip_err_t unzip_entry_info(int index, unzip_entry_t *out);

/**
 * @brief Extract entry @p index into @p dest_folder, creating intermediate
 * directories as needed. Directory entries just create the folder. Entry paths
 * are sanitized (no absolute paths or ".." traversal).
 */
unzip_err_t unzip_extract_entry(int index, const char *dest_folder);

/**
 * @brief Close the archive and release the underlying file handle.
 */
void unzip_close(void);

/**
 * @brief Human-readable string for an error code.
 */
const char *unzip_err_str(unzip_err_t err);

// --------------------------------------------------------------------------
// Background extraction job (main-loop driven, cancellable) — mirrors copy.c.
// One extraction runs at a time. It extracts one archive entry per poll.
// --------------------------------------------------------------------------

#define UNZIP_PATH_MAX 448

typedef enum {
  UNZIP_JOB_IDLE = 0,
  UNZIP_JOB_EXTRACTING,
  UNZIP_JOB_COMPLETED,
  UNZIP_JOB_FAILED,
  UNZIP_JOB_CANCELLED
} unzip_job_status_t;

typedef struct {
  unzip_job_status_t status;
  char archive[UNZIP_PATH_MAX];       // source .zip path
  char dest[UNZIP_PATH_MAX];          // destination folder
  char current_name[UNZIP_NAME_MAX];  // entry being extracted
  int entries_total;
  int entries_done;
  int entries_skipped;  // unsupported entries skipped over
  bool cancel_requested;
  unzip_err_t last_error;
} unzip_job_info_t;

/**
 * @brief Start extracting @p zip_path into @p dest_folder as a background job.
 * Returns false if a job is already active or the archive can't be opened
 * (unzip_job_get_info then carries the reason).
 */
bool unzip_job_start(const char *zip_path, const char *dest_folder);

/**
 * @brief Advance the job by one entry. Call from the main loop while
 * unzip_job_is_active() is true.
 */
void unzip_job_poll(void);

/**
 * @brief Request cancellation; the job stops at the next poll.
 */
void unzip_job_cancel(void);

/**
 * @brief True while an extraction is in progress.
 */
bool unzip_job_is_active(void);

/**
 * @brief Reset a finished job (completed/failed/cancelled) back to idle.
 */
void unzip_job_reset(void);

/**
 * @brief Snapshot the current job state.
 */
void unzip_job_get_info(unzip_job_info_t *out);

/**
 * @brief Human-readable string for a job status.
 */
const char *unzip_job_status_str(unzip_job_status_t status);

#endif  // UNZIP_H
