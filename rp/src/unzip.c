/**
 * File: unzip.c
 * Author: Diego Parrilla Santamaría
 * Date: July 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: FatFs-backed .zip extraction over the vendored miniz reader.
 */

#include "include/unzip.h"

#include <string.h>

#include "ff.h"
#include "include/debug.h"
#include "miniz.h"

// Single archive open at a time (matches download.c / copy.c).
static bool archiveOpen = false;
static FIL zipFile;
static mz_zip_archive zip;

// miniz read callback: pull archive bytes from the FatFs file.
static size_t zipReadCb(void *pOpaque, mz_uint64 fileOfs, void *pBuf,
                        size_t n) {
  FIL *fp = (FIL *)pOpaque;
  if (f_lseek(fp, (FSIZE_t)fileOfs) != FR_OK) {
    return 0;
  }
  UINT br = 0;
  if (f_read(fp, pBuf, (UINT)n, &br) != FR_OK) {
    return 0;
  }
  return br;
}

// miniz write callback: stream an entry's inflated bytes to the dest file.
// miniz calls this sequentially per entry, so a plain f_write is correct.
static size_t zipWriteCb(void *pOpaque, mz_uint64 fileOfs, const void *pBuf,
                         size_t n) {
  (void)fileOfs;
  FIL *out = (FIL *)pOpaque;
  UINT bw = 0;
  if (f_write(out, pBuf, (UINT)n, &bw) != FR_OK) {
    return 0;
  }
  return bw;
}

unzip_err_t unzip_open(const char *zip_path) {
  if (archiveOpen) {
    return UNZIP_ERR_STATE;
  }
  FRESULT fr = f_open(&zipFile, zip_path, FA_READ);
  if (fr != FR_OK) {
    DPRINTF("unzip: cannot open %s: %d\n", zip_path, fr);
    return UNZIP_ERR_OPEN;
  }

  memset(&zip, 0, sizeof(zip));
  zip.m_pRead = zipReadCb;
  zip.m_pIO_opaque = &zipFile;

  FSIZE_t size = f_size(&zipFile);
  if (!mz_zip_reader_init(&zip, (mz_uint64)size, 0)) {
    DPRINTF("unzip: not a valid zip (%s)\n", zip_path);
    f_close(&zipFile);
    return UNZIP_ERR_FORMAT;
  }
  archiveOpen = true;
  return UNZIP_OK;
}

int unzip_num_entries(void) {
  if (!archiveOpen) {
    return -1;
  }
  return (int)mz_zip_reader_get_num_files(&zip);
}

unzip_err_t unzip_entry_info(int index, unzip_entry_t *out) {
  if (!archiveOpen || out == NULL) {
    return UNZIP_ERR_STATE;
  }
  mz_zip_archive_file_stat st;
  if (!mz_zip_reader_file_stat(&zip, (mz_uint)index, &st)) {
    return UNZIP_ERR_FORMAT;
  }
  memset(out, 0, sizeof(*out));
  strncpy(out->name, st.m_filename, sizeof(out->name) - 1);
  out->uncomp_size = (uint64_t)st.m_uncomp_size;
  out->is_dir = st.m_is_directory ? true : false;
  // Supported: stored (0) or deflated (8), and not encrypted (bit 0).
  bool encrypted = (st.m_bit_flag & 1U) != 0U;
  bool method_ok = (st.m_method == 0U || st.m_method == 8U);
  out->supported = method_ok && !encrypted;
  return UNZIP_OK;
}

// Sanitize a zip entry name into a FatFs-safe relative path under a folder.
// Rejects absolute paths and ".." traversal; drops "." components; replaces
// FAT-illegal characters with '_'. Returns false if the result is empty or
// would overflow. Any trailing '/' (directory marker) is preserved via
// is_dir handling by the caller, not here.
static bool sanitize_rel_path(const char *name, char *out, size_t outSize) {
  char tmp[UNZIP_NAME_MAX];
  // Copy and normalize backslashes to forward slashes.
  size_t j = 0;
  for (size_t i = 0; name[i] != '\0' && j < sizeof(tmp) - 1; i++) {
    tmp[j++] = (name[i] == '\\') ? '/' : name[i];
  }
  tmp[j] = '\0';

  out[0] = '\0';
  size_t outLen = 0;
  const char *p = tmp;
  while (*p != '\0') {
    // Skip consecutive/leading slashes (also rejects absolute leading '/').
    while (*p == '/') {
      p++;
    }
    if (*p == '\0') {
      break;
    }
    // Component up to the next '/'.
    const char *end = p;
    while (*end != '\0' && *end != '/') {
      end++;
    }
    size_t compLen = (size_t)(end - p);
    if (compLen == 1 && p[0] == '.') {
      p = end;
      continue;  // drop "."
    }
    if (compLen == 2 && p[0] == '.' && p[1] == '.') {
      return false;  // reject traversal
    }
    // Append separator between components.
    if (outLen > 0) {
      if (outLen + 1 >= outSize) {
        return false;
      }
      out[outLen++] = '/';
    }
    // Append the component, replacing FAT-illegal characters.
    for (size_t k = 0; k < compLen; k++) {
      if (outLen + 1 >= outSize) {
        return false;
      }
      char c = p[k];
      if (strchr(":*?\"<>|", c) != NULL) {
        c = '_';
      }
      out[outLen++] = c;
    }
    p = end;
  }
  out[outLen] = '\0';
  return outLen > 0;
}

// Create every parent directory of a full file path (mkdir -p of the dirname).
// Existing directories are fine.
static unzip_err_t make_parent_dirs(const char *fullPath) {
  char dir[UNZIP_NAME_MAX * 2];
  strncpy(dir, fullPath, sizeof(dir) - 1);
  dir[sizeof(dir) - 1] = '\0';
  for (char *s = dir + 1; *s != '\0'; s++) {
    if (*s == '/') {
      *s = '\0';
      FRESULT fr = f_mkdir(dir);
      if (fr != FR_OK && fr != FR_EXIST) {
        DPRINTF("unzip: mkdir %s failed: %d\n", dir, fr);
        return UNZIP_ERR_WRITE;
      }
      *s = '/';
    }
  }
  return UNZIP_OK;
}

unzip_err_t unzip_extract_entry(int index, const char *dest_folder) {
  if (!archiveOpen) {
    return UNZIP_ERR_STATE;
  }
  unzip_entry_t info;
  unzip_err_t rc = unzip_entry_info(index, &info);
  if (rc != UNZIP_OK) {
    return rc;
  }
  if (!info.supported) {
    return UNZIP_ERR_UNSUPPORTED;
  }

  char rel[UNZIP_NAME_MAX];
  if (!sanitize_rel_path(info.name, rel, sizeof(rel))) {
    return UNZIP_ERR_PATH;
  }

  char full[UNZIP_NAME_MAX * 2];
  const char *base = dest_folder ? dest_folder : "/";
  int n = snprintf(full, sizeof(full), "%s/%s", base, rel);
  if (n <= 0 || (size_t)n >= sizeof(full)) {
    return UNZIP_ERR_PATH;
  }

  if (info.is_dir) {
    unzip_err_t mk = make_parent_dirs(full);
    if (mk != UNZIP_OK) {
      return mk;
    }
    FRESULT fr = f_mkdir(full);
    return (fr == FR_OK || fr == FR_EXIST) ? UNZIP_OK : UNZIP_ERR_WRITE;
  }

  unzip_err_t mk = make_parent_dirs(full);
  if (mk != UNZIP_OK) {
    return mk;
  }

  FIL out;
  FRESULT fr = f_open(&out, full, FA_WRITE | FA_CREATE_ALWAYS);
  if (fr != FR_OK) {
    DPRINTF("unzip: cannot create %s: %d\n", full, fr);
    return UNZIP_ERR_WRITE;
  }

  mz_bool ok =
      mz_zip_reader_extract_to_callback(&zip, (mz_uint)index, zipWriteCb, &out,
                                        0);
  FRESULT closeRc = f_close(&out);
  if (!ok) {
    DPRINTF("unzip: extract %s failed (miniz err %d)\n", full,
            mz_zip_get_last_error(&zip));
    f_unlink(full);  // don't leave a partial file behind
    return UNZIP_ERR_WRITE;
  }
  if (closeRc != FR_OK) {
    return UNZIP_ERR_WRITE;
  }
  return UNZIP_OK;
}

void unzip_close(void) {
  if (!archiveOpen) {
    return;
  }
  mz_zip_reader_end(&zip);
  f_close(&zipFile);
  archiveOpen = false;
}

const char *unzip_err_str(unzip_err_t err) {
  switch (err) {
    case UNZIP_OK:
      return "OK";
    case UNZIP_ERR_OPEN:
      return "cannot open archive";
    case UNZIP_ERR_FORMAT:
      return "not a valid zip archive";
    case UNZIP_ERR_READ:
      return "read error";
    case UNZIP_ERR_WRITE:
      return "write error or out of space";
    case UNZIP_ERR_UNSUPPORTED:
      return "unsupported (encrypted or unknown method)";
    case UNZIP_ERR_PATH:
      return "unsafe entry path";
    case UNZIP_ERR_STATE:
      return "no archive open";
    default:
      return "unknown error";
  }
}

// --------------------------------------------------------------------------
// Background extraction job
// --------------------------------------------------------------------------

static unzip_job_info_t job = {.status = UNZIP_JOB_IDLE};

static void job_finish(unzip_job_status_t status, unzip_err_t err) {
  unzip_close();
  job.status = status;
  job.last_error = err;
}

bool unzip_job_start(const char *zip_path, const char *dest_folder) {
  if (job.status == UNZIP_JOB_EXTRACTING) {
    return false;  // one job at a time
  }
  memset(&job, 0, sizeof(job));
  snprintf(job.archive, sizeof(job.archive), "%s", zip_path ? zip_path : "");
  snprintf(job.dest, sizeof(job.dest), "%s", dest_folder ? dest_folder : "/");

  unzip_err_t rc = unzip_open(job.archive);
  if (rc != UNZIP_OK) {
    job.status = UNZIP_JOB_FAILED;
    job.last_error = rc;
    return false;
  }
  int n = unzip_num_entries();
  if (n < 0) {
    job_finish(UNZIP_JOB_FAILED, UNZIP_ERR_FORMAT);
    return false;
  }
  job.entries_total = n;
  job.entries_done = 0;
  job.entries_skipped = 0;
  job.status = UNZIP_JOB_EXTRACTING;
  return true;
}

void unzip_job_poll(void) {
  if (job.status != UNZIP_JOB_EXTRACTING) {
    return;
  }
  if (job.cancel_requested) {
    job_finish(UNZIP_JOB_CANCELLED, UNZIP_OK);
    return;
  }
  if (job.entries_done >= job.entries_total) {
    job_finish(UNZIP_JOB_COMPLETED, UNZIP_OK);
    return;
  }

  int idx = job.entries_done;
  unzip_entry_t info;
  unzip_err_t rc = unzip_entry_info(idx, &info);
  if (rc == UNZIP_OK) {
    snprintf(job.current_name, sizeof(job.current_name), "%s", info.name);
    if (!info.supported) {
      // Skip encrypted / unsupported-method entries rather than fail the
      // whole archive.
      job.entries_skipped++;
    } else {
      rc = unzip_extract_entry(idx, job.dest);
    }
  }

  if (rc != UNZIP_OK && rc != UNZIP_ERR_UNSUPPORTED) {
    job_finish(UNZIP_JOB_FAILED, rc);
    return;
  }

  job.entries_done++;
  if (job.entries_done >= job.entries_total) {
    job_finish(UNZIP_JOB_COMPLETED, UNZIP_OK);
  }
}

void unzip_job_cancel(void) {
  if (job.status == UNZIP_JOB_EXTRACTING) {
    job.cancel_requested = true;
  }
}

bool unzip_job_is_active(void) {
  return job.status == UNZIP_JOB_EXTRACTING;
}

void unzip_job_reset(void) {
  if (job.status != UNZIP_JOB_EXTRACTING) {
    memset(&job, 0, sizeof(job));
    job.status = UNZIP_JOB_IDLE;
  }
}

void unzip_job_get_info(unzip_job_info_t *out) {
  if (out != NULL) {
    *out = job;
  }
}

const char *unzip_job_status_str(unzip_job_status_t status) {
  switch (status) {
    case UNZIP_JOB_IDLE:
      return "idle";
    case UNZIP_JOB_EXTRACTING:
      return "extracting";
    case UNZIP_JOB_COMPLETED:
      return "completed";
    case UNZIP_JOB_FAILED:
      return "failed";
    case UNZIP_JOB_CANCELLED:
      return "cancelled";
    default:
      return "unknown";
  }
}
