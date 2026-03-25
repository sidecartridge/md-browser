/**
 * File: copy.c
 * Author: Diego Parrilla Santamaría
 * Date: March 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Background file and folder copy job support.
 */

#include "copy.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "debug.h"

#ifndef COPY_POLL_STEPS
#define COPY_POLL_STEPS 8
#endif

typedef struct {
  DIR dir;
  size_t src_len;
  size_t dst_len;
} copy_stack_frame_t;

typedef struct {
  copy_info_t info;
  copy_stack_frame_t *stack;
  FILINFO *entry_info;
  char scan_path[COPY_MAX_PATH_LEN];
  char copy_src_path[COPY_MAX_PATH_LEN];
  char copy_dst_path[COPY_MAX_PATH_LEN];
  FIL src_file;
  FIL dst_file;
  bool scan_initialized;
  bool copy_initialized;
  bool file_copy_active;
  bool src_file_open;
  bool dst_file_open;
  uint8_t depth;
  size_t current_file_src_parent_len;
  size_t current_file_dst_parent_len;
  uint8_t *buffer;
} copy_job_t;

static copy_job_t copyJob = {0};

static bool copy_copy_string(char *dst, size_t dst_len, const char *src) {
  if (!dst || !src || dst_len == 0) return false;
  size_t src_len = strlen(src);
  if (src_len >= dst_len) return false;
  memcpy(dst, src, src_len + 1);
  return true;
}

static void copy_restore_path(char *path, size_t path_len) {
  if (!path) return;
  path[path_len] = '\0';
}

static bool copy_append_name(char *path, size_t path_len, const char *name,
                             size_t *new_len) {
  if (!path || !name || !new_len) return false;

  size_t len = strlen(path);
  int written = snprintf(path + len, path_len - len, "%s%s",
                         (len > 0 && path[len - 1] != '/') ? "/" : "", name);
  if (written < 0 || (size_t)written >= (path_len - len)) return false;

  *new_len = len + (size_t)written;
  return true;
}

static bool copy_set_current_paths(const char *src, const char *dst) {
  return copy_copy_string(copyJob.info.current_path,
                          sizeof(copyJob.info.current_path), src) &&
         copy_copy_string(copyJob.info.current_dst_path,
                          sizeof(copyJob.info.current_dst_path), dst);
}

static void copy_clear_current_paths(void) {
  copyJob.info.current_path[0] = '\0';
  copyJob.info.current_dst_path[0] = '\0';
}

static void copy_close_active_file_handles(void) {
  if (copyJob.src_file_open) {
    (void)f_close(&copyJob.src_file);
    copyJob.src_file_open = false;
  }
  if (copyJob.dst_file_open) {
    (void)f_close(&copyJob.dst_file);
    copyJob.dst_file_open = false;
  }
  copyJob.file_copy_active = false;
  copyJob.info.current_file_size = 0;
  copyJob.info.current_file_done = 0;
}

static void copy_close_stack_dirs(void) {
  while (copyJob.depth > 0 && copyJob.stack) {
    copyJob.depth--;
    (void)f_closedir(&copyJob.stack[copyJob.depth].dir);
  }
}

static FRESULT copy_remove_tree(const char *path) {
  if (!path || path[0] == '\0') return FR_INVALID_PARAMETER;

  FILINFO *fno = copyJob.entry_info;
  bool owns_fno = false;
  if (!fno) {
    fno = calloc(1, sizeof(*fno));
    if (!fno) return FR_NOT_ENOUGH_CORE;
    owns_fno = true;
  }

  FRESULT fr = f_stat(path, fno);
  if (fr != FR_OK) goto cleanup;
  if (!(fno->fattrib & AM_DIR)) {
    fr = f_unlink(path);
    goto cleanup;
  }

  uint8_t depth = 0;
  copy_stack_frame_t *cleanup_stack =
      calloc(COPY_MAX_DIR_DEPTH, sizeof(copy_stack_frame_t));
  char *cleanup_path = calloc(1, COPY_MAX_PATH_LEN);
  if (!cleanup_stack || !cleanup_path) {
    fr = FR_NOT_ENOUGH_CORE;
    free(cleanup_path);
    free(cleanup_stack);
    goto cleanup;
  }

  if (!copy_copy_string(cleanup_path, COPY_MAX_PATH_LEN, path)) {
    fr = FR_INVALID_NAME;
    free(cleanup_path);
    free(cleanup_stack);
    goto cleanup;
  }

  fr = f_opendir(&cleanup_stack[0].dir, cleanup_path);
  if (fr != FR_OK) {
    free(cleanup_path);
    free(cleanup_stack);
    goto cleanup;
  }
  cleanup_stack[0].src_len = strlen(cleanup_path);
  depth = 1;

  while (depth > 0) {
    copy_stack_frame_t *frame = &cleanup_stack[depth - 1];
    fr = f_readdir(&frame->dir, fno);
    if (fr != FR_OK) break;

    if (fno->fname[0] == '\0') {
      (void)f_closedir(&frame->dir);
      fr = f_unlink(cleanup_path);
      if (fr != FR_OK) break;
      depth--;
      if (depth > 0) {
        cleanup_path[cleanup_stack[depth - 1].src_len] = '\0';
      }
      continue;
    }

    size_t child_len = 0;
    if (!copy_append_name(cleanup_path, COPY_MAX_PATH_LEN, fno->fname,
                          &child_len)) {
      fr = FR_INVALID_NAME;
      break;
    }

    if (fno->fattrib & AM_DIR) {
      if (depth >= COPY_MAX_DIR_DEPTH) {
        fr = FR_DENIED;
        break;
      }
      fr = f_opendir(&cleanup_stack[depth].dir, cleanup_path);
      if (fr != FR_OK) break;
      cleanup_stack[depth].src_len = child_len;
      depth++;
    } else {
      fr = f_unlink(cleanup_path);
      cleanup_path[frame->src_len] = '\0';
      if (fr != FR_OK) break;
    }
  }

  while (depth > 0) {
    depth--;
    (void)f_closedir(&cleanup_stack[depth].dir);
  }
  free(cleanup_path);
  free(cleanup_stack);
cleanup:
  if (owns_fno) free(fno);
  return fr;
}

static void copy_reset_job(void) {
  copy_close_active_file_handles();
  copy_close_stack_dirs();
  free(copyJob.entry_info);
  copyJob.entry_info = NULL;
  free(copyJob.buffer);
  copyJob.buffer = NULL;
  free(copyJob.stack);
  copyJob.stack = NULL;
  memset(&copyJob, 0, sizeof(copyJob));
}

static void copy_finish(copy_status_t status, FRESULT fr, const char *message,
                        bool cleanup_dst) {
  copy_close_active_file_handles();
  copy_close_stack_dirs();

  if (cleanup_dst) {
    FRESULT cleanup_result = copy_remove_tree(copyJob.info.dst_path);
    if (cleanup_result != FR_OK && cleanup_result != FR_NO_FILE &&
        cleanup_result != FR_NO_PATH) {
      DPRINTF("Copy cleanup failed for %s: %d\n", copyJob.info.dst_path,
              (int)cleanup_result);
    }
  }

  copyJob.scan_initialized = false;
  copyJob.copy_initialized = false;
  free(copyJob.entry_info);
  copyJob.entry_info = NULL;
  free(copyJob.buffer);
  copyJob.buffer = NULL;
  free(copyJob.stack);
  copyJob.stack = NULL;
  copyJob.info.status = status;
  copyJob.info.last_error = fr;
  copyJob.info.cancel_requested = false;
  copyJob.info.current_file_size = 0;
  copyJob.info.current_file_done = 0;
  copy_clear_current_paths();

  if (message) {
    (void)snprintf(copyJob.info.error_message,
                   sizeof(copyJob.info.error_message), "%s", message);
  } else {
    copyJob.info.error_message[0] = '\0';
  }
}

static void copy_fail(FRESULT fr, const char *message) {
  copy_finish(COPY_STATUS_FAILED, fr, message, true);
}

static void copy_cancel(void) {
  copy_finish(COPY_STATUS_CANCELLED, FR_INT_ERR,
              copyJob.info.operation == COPY_OPERATION_MOVE ? "move cancelled"
                                                            : "copy cancelled",
              true);
}

static bool copy_complete_success(void) {
  if (copyJob.info.operation == COPY_OPERATION_MOVE) {
    copy_set_current_paths(copyJob.info.src_path, copyJob.info.dst_path);
    FRESULT fr = copy_remove_tree(copyJob.info.src_path);
    if (fr != FR_OK) {
      copy_finish(COPY_STATUS_FAILED, fr,
                  "move completed copy phase but could not remove source",
                  false);
      return false;
    }
  }

  copyJob.info.status = COPY_STATUS_COMPLETED;
  copy_clear_current_paths();
  return true;
}

static bool copy_begin_scan(void) {
  if (!copy_copy_string(copyJob.scan_path, sizeof(copyJob.scan_path),
                        copyJob.info.src_path)) {
    copy_fail(FR_INVALID_NAME, "path too long");
    return false;
  }

  copyJob.info.dirs_total = 1;
  copyJob.depth = 0;

  FRESULT fr = f_opendir(&copyJob.stack[0].dir, copyJob.scan_path);
  if (fr != FR_OK) {
    copy_fail(fr, "cannot open source folder");
    return false;
  }

  copyJob.stack[0].src_len = strlen(copyJob.scan_path);
  copyJob.depth = 1;
  copyJob.scan_initialized = true;
  copy_copy_string(copyJob.info.current_path, sizeof(copyJob.info.current_path),
                   copyJob.info.src_path);
  return true;
}

static bool copy_open_current_file(uint64_t file_size) {
  FRESULT fr = f_open(&copyJob.src_file, copyJob.copy_src_path, FA_READ);
  if (fr != FR_OK) {
    copy_fail(fr, "cannot open source file");
    return false;
  }
  copyJob.src_file_open = true;

  fr = f_open(&copyJob.dst_file, copyJob.copy_dst_path, FA_CREATE_NEW | FA_WRITE);
  if (fr != FR_OK) {
    copy_fail(fr, "cannot create destination file");
    return false;
  }
  copyJob.dst_file_open = true;
  copyJob.file_copy_active = true;
  copyJob.info.current_file_size = file_size;
  copyJob.info.current_file_done = 0;
  copy_set_current_paths(copyJob.copy_src_path, copyJob.copy_dst_path);
  return true;
}

static bool copy_begin_copy(void) {
  if (!copy_copy_string(copyJob.copy_src_path, sizeof(copyJob.copy_src_path),
                        copyJob.info.src_path) ||
      !copy_copy_string(copyJob.copy_dst_path, sizeof(copyJob.copy_dst_path),
                        copyJob.info.dst_path)) {
    copy_fail(FR_INVALID_NAME, "path too long");
    return false;
  }

  copyJob.depth = 0;
  copyJob.copy_initialized = true;

  if (copyJob.info.src_is_dir) {
    FRESULT fr = f_mkdir(copyJob.copy_dst_path);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot create destination folder");
      return false;
    }

    copyJob.info.dirs_done = 1;
    copy_set_current_paths(copyJob.info.src_path, copyJob.info.dst_path);

    fr = f_opendir(&copyJob.stack[0].dir, copyJob.copy_src_path);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot open source folder");
      return false;
    }

    copyJob.stack[0].src_len = strlen(copyJob.copy_src_path);
    copyJob.stack[0].dst_len = strlen(copyJob.copy_dst_path);
    copyJob.depth = 1;
    return true;
  }

  copyJob.current_file_src_parent_len = strlen(copyJob.copy_src_path);
  copyJob.current_file_dst_parent_len = strlen(copyJob.copy_dst_path);
  return copy_open_current_file(copyJob.info.bytes_total);
}

static bool copy_process_scan(void) {
  if (!copyJob.scan_initialized && !copy_begin_scan()) return false;
  if (!copyJob.entry_info) {
    copy_fail(FR_NOT_ENOUGH_CORE, "copy state not initialized");
    return false;
  }

  for (int step = 0; step < COPY_POLL_STEPS; step++) {
    if (copyJob.info.cancel_requested) {
      copy_cancel();
      return false;
    }
    if (copyJob.depth == 0) {
      copyJob.scan_initialized = false;
      copyJob.info.status = COPY_STATUS_COPYING;
      return true;
    }

    copy_stack_frame_t *frame = &copyJob.stack[copyJob.depth - 1];
    FILINFO *fno = copyJob.entry_info;
    FRESULT fr = f_readdir(&frame->dir, fno);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot scan source folder");
      return false;
    }

    if (fno->fname[0] == '\0') {
      (void)f_closedir(&frame->dir);
      copyJob.depth--;
      if (copyJob.depth > 0) {
        copy_restore_path(copyJob.scan_path, copyJob.stack[copyJob.depth - 1].src_len);
      }
      continue;
    }

    if (fno->fattrib & AM_DIR) {
      if (copyJob.depth >= COPY_MAX_DIR_DEPTH) {
        copy_fail(FR_DENIED, "directory nesting too deep");
        return false;
      }

      size_t child_len = 0;
      if (!copy_append_name(copyJob.scan_path, sizeof(copyJob.scan_path),
                            fno->fname, &child_len)) {
        copy_fail(FR_INVALID_NAME, "path too long");
        return false;
      }

      copyJob.info.dirs_total++;
      (void)copy_copy_string(copyJob.info.current_path,
                             sizeof(copyJob.info.current_path),
                             copyJob.scan_path);

      fr = f_opendir(&copyJob.stack[copyJob.depth].dir, copyJob.scan_path);
      if (fr != FR_OK) {
        copy_fail(fr, "cannot open nested folder");
        return false;
      }

      copyJob.stack[copyJob.depth].src_len = child_len;
      copyJob.depth++;
      continue;
    }

    copyJob.info.files_total++;
    copyJob.info.bytes_total += (uint64_t)fno->fsize;
    if (!copy_append_name(copyJob.scan_path, sizeof(copyJob.scan_path),
                          fno->fname, &frame->dst_len)) {
      copy_fail(FR_INVALID_NAME, "path too long");
      return false;
    }
    (void)copy_copy_string(copyJob.info.current_path,
                           sizeof(copyJob.info.current_path),
                           copyJob.scan_path);
    copy_restore_path(copyJob.scan_path, frame->src_len);
  }

  return true;
}

static bool copy_process_file_chunk(void) {
  UINT bytes_read = 0;
  FRESULT fr = f_read(&copyJob.src_file, copyJob.buffer, COPY_CHUNK_SIZE,
                      &bytes_read);
  if (fr != FR_OK) {
    copy_fail(fr, "cannot read source file");
    return false;
  }

  if (bytes_read == 0) {
    fr = f_sync(&copyJob.dst_file);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot flush destination file");
      return false;
    }

    copy_close_active_file_handles();
    copyJob.info.files_done++;
    if (copyJob.info.bytes_done > copyJob.info.bytes_total) {
      copyJob.info.bytes_done = copyJob.info.bytes_total;
    }

    if (copyJob.info.src_is_dir) {
      copy_restore_path(copyJob.copy_src_path, copyJob.current_file_src_parent_len);
      copy_restore_path(copyJob.copy_dst_path, copyJob.current_file_dst_parent_len);
    } else {
      return copy_complete_success();
    }

    return true;
  }

  UINT bytes_written = 0;
  fr = f_write(&copyJob.dst_file, copyJob.buffer, bytes_read, &bytes_written);
  if (fr != FR_OK || bytes_written != bytes_read) {
    copy_fail(fr == FR_OK ? FR_DISK_ERR : fr, "cannot write destination file");
    return false;
  }

  copyJob.info.bytes_done += (uint64_t)bytes_written;
  copyJob.info.current_file_done += (uint64_t)bytes_written;
  return true;
}

static bool copy_process_copy(void) {
  if (!copyJob.copy_initialized && !copy_begin_copy()) return false;
  if (!copyJob.entry_info) {
    copy_fail(FR_NOT_ENOUGH_CORE, "copy state not initialized");
    return false;
  }

  for (int step = 0; step < COPY_POLL_STEPS; step++) {
    if (copyJob.info.cancel_requested) {
      copy_cancel();
      return false;
    }

    if (copyJob.file_copy_active) {
      if (!copy_process_file_chunk()) return false;
      if (copyJob.info.status == COPY_STATUS_COMPLETED) return true;
      continue;
    }

    if (!copyJob.info.src_is_dir) {
      if (copyJob.info.status != COPY_STATUS_COMPLETED &&
          !copy_open_current_file(copyJob.info.bytes_total)) {
        return false;
      }
      continue;
    }

    if (copyJob.depth == 0) {
      return copy_complete_success();
    }

    copy_stack_frame_t *frame = &copyJob.stack[copyJob.depth - 1];
    FILINFO *fno = copyJob.entry_info;
    FRESULT fr = f_readdir(&frame->dir, fno);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot read source folder");
      return false;
    }

    if (fno->fname[0] == '\0') {
      (void)f_closedir(&frame->dir);
      copyJob.depth--;
      if (copyJob.depth > 0) {
        copy_restore_path(copyJob.copy_src_path,
                          copyJob.stack[copyJob.depth - 1].src_len);
        copy_restore_path(copyJob.copy_dst_path,
                          copyJob.stack[copyJob.depth - 1].dst_len);
      }
      continue;
    }

    size_t child_src_len = 0;
    size_t child_dst_len = 0;
    if (!copy_append_name(copyJob.copy_src_path, sizeof(copyJob.copy_src_path),
                          fno->fname, &child_src_len) ||
        !copy_append_name(copyJob.copy_dst_path, sizeof(copyJob.copy_dst_path),
                          fno->fname, &child_dst_len)) {
      copy_fail(FR_INVALID_NAME, "path too long");
      return false;
    }

    if (fno->fattrib & AM_DIR) {
      if (copyJob.depth >= COPY_MAX_DIR_DEPTH) {
        copy_fail(FR_DENIED, "directory nesting too deep");
        return false;
      }

      copy_set_current_paths(copyJob.copy_src_path, copyJob.copy_dst_path);
      fr = f_mkdir(copyJob.copy_dst_path);
      if (fr != FR_OK) {
        copy_fail(fr, "cannot create nested folder");
        return false;
      }

      copyJob.info.dirs_done++;
      fr = f_opendir(&copyJob.stack[copyJob.depth].dir, copyJob.copy_src_path);
      if (fr != FR_OK) {
        copy_fail(fr, "cannot open nested source folder");
        return false;
      }

      copyJob.stack[copyJob.depth].src_len = child_src_len;
      copyJob.stack[copyJob.depth].dst_len = child_dst_len;
      copyJob.depth++;
      continue;
    }

    copyJob.current_file_src_parent_len = frame->src_len;
    copyJob.current_file_dst_parent_len = frame->dst_len;
    if (!copy_open_current_file((uint64_t)fno->fsize)) return false;
  }

  return true;
}

static bool copy_start_internal(const char *src_path, const char *dst_path,
                                copy_operation_t operation) {
  if (!src_path || !dst_path || src_path[0] == '\0' || dst_path[0] == '\0') {
    return false;
  }
  if (copy_is_active()) return false;

  copy_reset_job();

  if (!copy_copy_string(copyJob.info.src_path, sizeof(copyJob.info.src_path),
                        src_path) ||
      !copy_copy_string(copyJob.info.dst_path, sizeof(copyJob.info.dst_path),
                        dst_path)) {
    copy_reset_job();
    return false;
  }

  copyJob.stack = calloc(COPY_MAX_DIR_DEPTH, sizeof(copy_stack_frame_t));
  copyJob.buffer = malloc(COPY_CHUNK_SIZE);
  copyJob.entry_info = calloc(1, sizeof(*copyJob.entry_info));
  if (!copyJob.stack || !copyJob.buffer || !copyJob.entry_info) {
    copy_reset_job();
    return false;
  }

  FRESULT fr = f_stat(copyJob.info.src_path, copyJob.entry_info);
  if (fr != FR_OK) {
    copy_reset_job();
    return false;
  }
  BYTE src_fattrib = copyJob.entry_info->fattrib;
  FSIZE_t src_fsize = copyJob.entry_info->fsize;

  fr = f_stat(copyJob.info.dst_path, copyJob.entry_info);
  if (fr == FR_OK) {
    copy_reset_job();
    return false;
  }
  if (fr != FR_NO_FILE && fr != FR_NO_PATH) {
    copy_reset_job();
    return false;
  }

  copyJob.info.operation = operation;
  copyJob.info.src_is_dir = ((src_fattrib & AM_DIR) != 0);
  copyJob.info.status = copyJob.info.src_is_dir ? COPY_STATUS_SCANNING
                                                : COPY_STATUS_COPYING;
  copyJob.info.last_error = FR_OK;
  copyJob.info.error_message[0] = '\0';
  copyJob.info.bytes_total =
      copyJob.info.src_is_dir ? 0 : (uint64_t)src_fsize;
  copyJob.info.files_total = copyJob.info.src_is_dir ? 0 : 1;
  copyJob.info.dirs_total = 0;
  copyJob.info.files_done = 0;
  copyJob.info.dirs_done = 0;
  copyJob.info.bytes_done = 0;
  copyJob.info.current_file_size = 0;
  copyJob.info.current_file_done = 0;
  copy_clear_current_paths();

  return true;
}

bool copy_start(const char *src_path, const char *dst_path) {
  return copy_start_internal(src_path, dst_path, COPY_OPERATION_COPY);
}

bool copy_start_move(const char *src_path, const char *dst_path) {
  return copy_start_internal(src_path, dst_path, COPY_OPERATION_MOVE);
}

bool copy_reset_status(void) {
  if (copy_is_active()) return false;
  copy_reset_job();
  return true;
}

void copy_poll(void) {
  switch (copyJob.info.status) {
    case COPY_STATUS_SCANNING:
      (void)copy_process_scan();
      break;
    case COPY_STATUS_COPYING:
      (void)copy_process_copy();
      break;
    default:
      break;
  }
}

void copy_request_cancel(void) {
  if (copy_is_active()) {
    copyJob.info.cancel_requested = true;
  }
}

bool copy_is_active(void) {
  return copyJob.info.status == COPY_STATUS_SCANNING ||
         copyJob.info.status == COPY_STATUS_COPYING;
}

copy_status_t copy_get_status(void) { return copyJob.info.status; }

void copy_get_info(copy_info_t *info) {
  if (!info) return;
  memcpy(info, &copyJob.info, sizeof(*info));
}

const char *copy_status_to_string(copy_status_t status) {
  switch (status) {
    case COPY_STATUS_IDLE:
      return "idle";
    case COPY_STATUS_SCANNING:
      return "scanning";
    case COPY_STATUS_COPYING:
      return "copying";
    case COPY_STATUS_COMPLETED:
      return "completed";
    case COPY_STATUS_FAILED:
      return "failed";
    case COPY_STATUS_CANCELLED:
      return "cancelled";
    default:
      return "unknown";
  }
}

const char *copy_operation_to_string(copy_operation_t operation) {
  switch (operation) {
    case COPY_OPERATION_COPY:
      return "copy";
    case COPY_OPERATION_MOVE:
      return "move";
    default:
      return "copy";
  }
}
