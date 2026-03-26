/**
 * File: copy.c
 * Author: Diego Parrilla Santamaría
 * Date: March 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Background file and folder copy job support.
 */

#include "copy.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "include/stfs.h"

#ifndef COPY_POLL_STEPS
#define COPY_POLL_STEPS 8
#endif

typedef enum {
  COPY_SOURCE_HOST = 0,
  COPY_SOURCE_IMAGE
} copy_source_t;

typedef enum {
  COPY_DEST_HOST = 0,
  COPY_DEST_IMAGE
} copy_destination_t;

typedef struct {
  DIR dir;
  size_t src_len;
  size_t dst_len;
} copy_host_stack_frame_t;

typedef struct {
  stfs_dir_t dir;
  size_t src_len;
  size_t dst_len;
} copy_image_stack_frame_t;

typedef struct {
  copy_info_t info;
  copy_source_t source;
  copy_destination_t destination;
  copy_host_stack_frame_t *host_stack;
  copy_image_stack_frame_t *image_stack;
  FILINFO *entry_info;
  stfs_dirent_t *image_entry_info;
  char source_path[COPY_MAX_PATH_LEN];
  char dest_path[COPY_MAX_PATH_LEN];
  char image_file_path[COPY_MAX_PATH_LEN];
  char scan_path[COPY_MAX_PATH_LEN];
  char copy_src_path[COPY_MAX_PATH_LEN];
  char copy_dst_path[COPY_MAX_PATH_LEN];
  FIL src_file;
  FIL dst_file;
  stfs_t image_fs;
  stfs_file_t image_src_file;
  stfs_write_file_t image_dst_file;
  bool image_fs_open;
  bool scan_initialized;
  bool copy_initialized;
  bool file_copy_active;
  bool src_file_open;
  bool image_src_file_open;
  bool dst_file_open;
  bool image_dst_file_open;
  uint8_t depth;
  size_t current_file_src_parent_len;
  size_t current_file_dst_parent_len;
  uint8_t *buffer;
} copy_job_t;

static bool copy_copy_string(char *dst, size_t dst_len, const char *src);

static copy_job_t copyJob = {0};
static FRESULT copyLastStartError = FR_OK;
static char copyLastStartErrorMessage[COPY_ERROR_MSG_LEN] = {0};

static void copy_clear_start_error(void) {
  copyLastStartError = FR_OK;
  copyLastStartErrorMessage[0] = '\0';
}

static void copy_set_start_error(FRESULT fr, const char *message) {
  copyLastStartError = fr;
  if (!message) {
    copyLastStartErrorMessage[0] = '\0';
    return;
  }

  if (!copy_copy_string(copyLastStartErrorMessage,
                        sizeof(copyLastStartErrorMessage), message)) {
    snprintf(copyLastStartErrorMessage, sizeof(copyLastStartErrorMessage),
             "start failed (%d)", (int)fr);
  }
}

static bool copy_copy_string(char *dst, size_t dst_len, const char *src) {
  size_t src_len = 0;

  if (!dst || !src || dst_len == 0u) return false;

  src_len = strlen(src);
  if (src_len >= dst_len) return false;
  memcpy(dst, src, src_len + 1u);
  return true;
}

static bool copy_is_image_source(void) {
  return copyJob.source == COPY_SOURCE_IMAGE;
}

static bool copy_is_image_dest(void) {
  return copyJob.destination == COPY_DEST_IMAGE;
}

static void copy_restore_path(char *path, size_t path_len) {
  if (!path) return;
  path[path_len] = '\0';
}

static bool copy_append_name(char *path, size_t path_len, const char *name,
                             size_t *new_len) {
  size_t len = 0;
  int written = 0;

  if (!path || !name || !new_len) return false;

  len = strlen(path);
  written = snprintf(path + len, path_len - len, "%s%s",
                     (len > 0u && path[len - 1u] != '/') ? "/" : "", name);
  if (written < 0 || (size_t)written >= (path_len - len)) return false;

  *new_len = len + (size_t)written;
  return true;
}

static bool copy_split_parent_child(const char *path, char *parent,
                                    size_t parent_len, char *name,
                                    size_t name_len) {
  const char *slash = NULL;
  size_t child_len = 0u;
  size_t parent_size = 0u;

  if (!path || !parent || !name || path[0] != '/') return false;
  if (strcmp(path, "/") == 0) return false;

  slash = strrchr(path, '/');
  if (!slash) return false;

  child_len = strlen(slash + 1u);
  if (child_len == 0u || child_len >= name_len) return false;

  parent_size = (size_t)(slash - path);
  if (parent_size == 0u) {
    if (!copy_copy_string(parent, parent_len, "/")) return false;
  } else {
    if (parent_size >= parent_len) return false;
    memcpy(parent, path, parent_size);
    parent[parent_size] = '\0';
  }

  memcpy(name, slash + 1u, child_len + 1u);
  return true;
}

static bool copy_resolve_image_child_path(const char *parent_inner_path,
                                          const char *source_name,
                                          char *resolved_path,
                                          size_t resolved_path_len) {
  char resolved_name[STFS_MAX_NAME_LEN];
  bool renamed = false;
  size_t ignored_len = 0u;
  FRESULT fr = FR_OK;

  if (!parent_inner_path || !source_name || !resolved_path) return false;

  fr = stfs_resolve_sfn_name(&copyJob.image_fs, parent_inner_path, source_name,
                             resolved_name, sizeof(resolved_name), &renamed);
  if (fr != FR_OK) return false;
  (void)renamed;

  if (!copy_copy_string(resolved_path, resolved_path_len, parent_inner_path)) {
    return false;
  }

  return copy_append_name(resolved_path, resolved_path_len, resolved_name,
                          &ignored_len);
}

static const char *copy_basename(const char *path) {
  const char *base = NULL;

  if (!path || path[0] == '\0') return "";

  base = strrchr(path, '/');
  return (base && base[1] != '\0') ? (base + 1) : path;
}

static bool copy_format_image_display_path(char *dst, size_t dst_len,
                                           const char *image_file_path,
                                           const char *inner_path) {
  const char *image_name = NULL;
  const char *path = NULL;
  int written = 0;

  if (!dst || !image_file_path || !inner_path || dst_len == 0u) return false;

  image_name = copy_basename(image_file_path);
  path = (inner_path[0] != '\0') ? inner_path : "/";
  written = snprintf(dst, dst_len, "%s @ %s", image_name, path);
  return written >= 0 && (size_t)written < dst_len;
}

static bool copy_set_current_paths(const char *src, const char *dst) {
  return copy_copy_string(copyJob.info.current_path,
                          sizeof(copyJob.info.current_path), src) &&
         copy_copy_string(copyJob.info.current_dst_path,
                          sizeof(copyJob.info.current_dst_path), dst);
}

static bool copy_set_current_image_paths(const char *inner_src,
                                         const char *dst_path) {
  return copy_format_image_display_path(copyJob.info.current_path,
                                        sizeof(copyJob.info.current_path),
                                        copyJob.image_file_path, inner_src) &&
         copy_copy_string(copyJob.info.current_dst_path,
                          sizeof(copyJob.info.current_dst_path), dst_path);
}

static bool copy_set_current_host_to_image_paths(const char *src_path,
                                                 const char *inner_dst) {
  return copy_copy_string(copyJob.info.current_path,
                          sizeof(copyJob.info.current_path), src_path) &&
         copy_format_image_display_path(copyJob.info.current_dst_path,
                                        sizeof(copyJob.info.current_dst_path),
                                        copyJob.image_file_path, inner_dst);
}

static void copy_clear_current_paths(void) {
  copyJob.info.current_path[0] = '\0';
  copyJob.info.current_dst_path[0] = '\0';
}

static void copy_abort_active_file_handles(void) {
  if (copyJob.src_file_open) {
    (void)f_close(&copyJob.src_file);
    copyJob.src_file_open = false;
  }
  if (copyJob.image_src_file_open) {
    stfs_close_file(&copyJob.image_src_file);
    copyJob.image_src_file_open = false;
  }
  if (copyJob.dst_file_open) {
    (void)f_close(&copyJob.dst_file);
    copyJob.dst_file_open = false;
  }
  if (copyJob.image_dst_file_open) {
    (void)stfs_close_write_file(&copyJob.image_dst_file, false);
    copyJob.image_dst_file_open = false;
  }
  copyJob.file_copy_active = false;
  copyJob.info.current_file_size = 0;
  copyJob.info.current_file_done = 0;
}

static void copy_close_stack_dirs(void) {
  if (copy_is_image_source()) {
    while (copyJob.depth > 0u && copyJob.image_stack) {
      copyJob.depth--;
      stfs_closedir(&copyJob.image_stack[copyJob.depth].dir);
    }
    return;
  }

  while (copyJob.depth > 0u && copyJob.host_stack) {
    copyJob.depth--;
    (void)f_closedir(&copyJob.host_stack[copyJob.depth].dir);
  }
}

static FRESULT copy_remove_tree(const char *path) {
  uint8_t depth = 0;
  copy_host_stack_frame_t *cleanup_stack = NULL;
  char *cleanup_path = NULL;
  FILINFO *fno = copyJob.entry_info;
  bool owns_fno = false;
  FRESULT fr = FR_OK;

  if (!path || path[0] == '\0') return FR_INVALID_PARAMETER;

  if (!fno) {
    fno = calloc(1, sizeof(*fno));
    if (!fno) return FR_NOT_ENOUGH_CORE;
    owns_fno = true;
  }

  fr = f_stat(path, fno);
  if (fr != FR_OK) goto cleanup;
  if (!(fno->fattrib & AM_DIR)) {
    fr = f_unlink(path);
    goto cleanup;
  }

  cleanup_stack = calloc(COPY_MAX_DIR_DEPTH, sizeof(*cleanup_stack));
  cleanup_path = calloc(1, COPY_MAX_PATH_LEN);
  if (!cleanup_stack || !cleanup_path) {
    fr = FR_NOT_ENOUGH_CORE;
    goto cleanup;
  }

  if (!copy_copy_string(cleanup_path, COPY_MAX_PATH_LEN, path)) {
    fr = FR_INVALID_NAME;
    goto cleanup;
  }

  fr = f_opendir(&cleanup_stack[0].dir, cleanup_path);
  if (fr != FR_OK) goto cleanup;
  cleanup_stack[0].src_len = strlen(cleanup_path);
  depth = 1u;

  while (depth > 0u) {
    copy_host_stack_frame_t *frame = &cleanup_stack[depth - 1u];

    fr = f_readdir(&frame->dir, fno);
    if (fr != FR_OK) break;

    if (fno->fname[0] == '\0') {
      (void)f_closedir(&frame->dir);
      fr = f_unlink(cleanup_path);
      if (fr != FR_OK) break;
      depth--;
      if (depth > 0u) {
        cleanup_path[cleanup_stack[depth - 1u].src_len] = '\0';
      }
      continue;
    }

    {
      size_t child_len = 0u;
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
  }

cleanup:
  while (depth > 0u && cleanup_stack) {
    depth--;
    (void)f_closedir(&cleanup_stack[depth].dir);
  }
  free(cleanup_path);
  free(cleanup_stack);
  if (owns_fno) free(fno);
  return fr;
}

static void copy_release_resources(void) {
  copy_abort_active_file_handles();
  copy_close_stack_dirs();
  if (copyJob.image_fs_open) {
    stfs_close(&copyJob.image_fs);
    copyJob.image_fs_open = false;
  }
  free(copyJob.image_entry_info);
  copyJob.image_entry_info = NULL;
  free(copyJob.entry_info);
  copyJob.entry_info = NULL;
  free(copyJob.buffer);
  copyJob.buffer = NULL;
  free(copyJob.host_stack);
  copyJob.host_stack = NULL;
  free(copyJob.image_stack);
  copyJob.image_stack = NULL;
  copyJob.scan_initialized = false;
  copyJob.copy_initialized = false;
  copyJob.depth = 0u;
}

static void copy_reset_job(void) {
  copy_release_resources();
  memset(&copyJob, 0, sizeof(copyJob));
}

static void copy_finish(copy_status_t status, FRESULT fr, const char *message,
                        bool cleanup_dst) {
  copy_abort_active_file_handles();
  copy_close_stack_dirs();

  if (cleanup_dst && copyJob.dest_path[0] != '\0') {
    FRESULT cleanup_result =
        copy_is_image_dest() && copyJob.image_fs_open
            ? stfs_delete_tree(&copyJob.image_fs, copyJob.dest_path)
            : copy_remove_tree(copyJob.dest_path);
    if (cleanup_result != FR_OK && cleanup_result != FR_NO_FILE &&
        cleanup_result != FR_NO_PATH) {
      DPRINTF("Copy cleanup failed for %s: %d\n", copyJob.dest_path,
              (int)cleanup_result);
    }
  }

  copy_release_resources();

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
  if (copyJob.info.operation == COPY_OPERATION_MOVE &&
      copyJob.source == COPY_SOURCE_HOST) {
    copy_set_current_paths(copyJob.source_path, copyJob.info.dst_path);
    if (f_unlink(copyJob.source_path) != FR_OK) {
      FRESULT fr = copy_remove_tree(copyJob.source_path);
      if (fr != FR_OK) {
        copy_finish(COPY_STATUS_FAILED, fr,
                    "move completed copy phase but could not remove source",
                    false);
        return false;
      }
    }
  }

  copy_release_resources();
  copyJob.info.status = COPY_STATUS_COMPLETED;
  copyJob.info.cancel_requested = false;
  copyJob.info.last_error = FR_OK;
  copyJob.info.error_message[0] = '\0';
  copy_clear_current_paths();
  return true;
}

static bool copy_begin_scan_host(void) {
  FRESULT fr = FR_OK;

  if (!copy_copy_string(copyJob.scan_path, sizeof(copyJob.scan_path),
                        copyJob.source_path)) {
    copy_fail(FR_INVALID_NAME, "path too long");
    return false;
  }

  copyJob.info.dirs_total = 1u;
  copyJob.depth = 0u;

  fr = f_opendir(&copyJob.host_stack[0].dir, copyJob.scan_path);
  if (fr != FR_OK) {
    copy_fail(fr, "cannot open source folder");
    return false;
  }

  copyJob.host_stack[0].src_len = strlen(copyJob.scan_path);
  copyJob.depth = 1u;
  copyJob.scan_initialized = true;
  (void)copy_copy_string(copyJob.info.current_path,
                         sizeof(copyJob.info.current_path),
                         copyJob.info.src_path);
  return true;
}

static bool copy_begin_scan_image(void) {
  FRESULT fr = FR_OK;

  if (!copy_copy_string(copyJob.scan_path, sizeof(copyJob.scan_path),
                        copyJob.source_path)) {
    copy_fail(FR_INVALID_NAME, "path too long");
    return false;
  }

  copyJob.info.dirs_total = 1u;
  copyJob.depth = 0u;

  fr = stfs_opendir(&copyJob.image_fs, copyJob.scan_path,
                    &copyJob.image_stack[0].dir);
  if (fr != FR_OK) {
    copy_fail(fr, "cannot open image folder");
    return false;
  }

  copyJob.image_stack[0].src_len = strlen(copyJob.scan_path);
  copyJob.depth = 1u;
  copyJob.scan_initialized = true;
  (void)copy_set_current_image_paths(copyJob.source_path, copyJob.info.dst_path);
  return true;
}

static bool copy_open_current_host_file(uint64_t file_size) {
  FRESULT fr = f_open(&copyJob.src_file, copyJob.copy_src_path, FA_READ);
  if (fr != FR_OK) {
    copy_fail(fr, "cannot open source file");
    return false;
  }
  copyJob.src_file_open = true;

  if (copy_is_image_dest()) {
    char parent_path[COPY_MAX_PATH_LEN];
    char name[STFS_MAX_NAME_LEN];

    if (!copyJob.entry_info ||
        !copy_split_parent_child(copyJob.copy_dst_path, parent_path,
                                 sizeof(parent_path), name, sizeof(name))) {
      copy_fail(FR_INVALID_NAME, "invalid image destination path");
      return false;
    }

    fr = stfs_create_file(&copyJob.image_fs, parent_path, name,
                          (DWORD)file_size,
                          (BYTE)(copyJob.entry_info->fattrib & ~AM_DIR),
                          copyJob.entry_info->fdate, copyJob.entry_info->ftime,
                          &copyJob.image_dst_file);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot create destination file");
      return false;
    }
    copyJob.image_dst_file_open = true;
    copy_set_current_host_to_image_paths(copyJob.copy_src_path,
                                         copyJob.copy_dst_path);
  } else {
    fr =
        f_open(&copyJob.dst_file, copyJob.copy_dst_path, FA_CREATE_NEW | FA_WRITE);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot create destination file");
      return false;
    }

    copyJob.dst_file_open = true;
    copy_set_current_paths(copyJob.copy_src_path, copyJob.copy_dst_path);
  }

  copyJob.file_copy_active = true;
  copyJob.info.current_file_size = file_size;
  copyJob.info.current_file_done = 0u;
  return true;
}

static bool copy_open_current_image_file(uint64_t file_size) {
  FRESULT fr = stfs_open_file(&copyJob.image_fs, copyJob.copy_src_path,
                              &copyJob.image_src_file);
  if (fr != FR_OK) {
    copy_fail(fr, "cannot open image file");
    return false;
  }
  copyJob.image_src_file_open = true;

  fr = f_open(&copyJob.dst_file, copyJob.copy_dst_path, FA_CREATE_NEW | FA_WRITE);
  if (fr != FR_OK) {
    copy_fail(fr, "cannot create destination file");
    return false;
  }

  copyJob.dst_file_open = true;
  copyJob.file_copy_active = true;
  copyJob.info.current_file_size = file_size;
  copyJob.info.current_file_done = 0u;
  copy_set_current_image_paths(copyJob.copy_src_path, copyJob.copy_dst_path);
  return true;
}

static bool copy_begin_copy_host(void) {
  if (copy_is_image_dest()) {
    FRESULT fr = FR_OK;

    if (!copy_copy_string(copyJob.copy_src_path, sizeof(copyJob.copy_src_path),
                          copyJob.source_path) ||
        !copy_copy_string(copyJob.copy_dst_path, sizeof(copyJob.copy_dst_path),
                          copyJob.dest_path)) {
      copy_fail(FR_INVALID_NAME, "path too long");
      return false;
    }

    copyJob.depth = 0u;
    copyJob.copy_initialized = true;

    if (copyJob.info.src_is_dir) {
      char parent_path[COPY_MAX_PATH_LEN];
      char name[STFS_MAX_NAME_LEN];

      if (!copy_split_parent_child(copyJob.copy_dst_path, parent_path,
                                   sizeof(parent_path), name,
                                   sizeof(name))) {
        copy_fail(FR_INVALID_NAME, "invalid image destination path");
        return false;
      }

      fr = stfs_mkdir(&copyJob.image_fs, parent_path, name,
                      (BYTE)(copyJob.entry_info->fattrib & ~AM_DIR),
                      copyJob.entry_info->fdate, copyJob.entry_info->ftime);
      if (fr != FR_OK) {
        copy_fail(fr, "cannot create destination folder");
        return false;
      }

      copyJob.info.dirs_done = 1u;
      copy_set_current_host_to_image_paths(copyJob.info.src_path,
                                           copyJob.copy_dst_path);

      fr = f_opendir(&copyJob.host_stack[0].dir, copyJob.copy_src_path);
      if (fr != FR_OK) {
        copy_fail(fr, "cannot open source folder");
        return false;
      }

      copyJob.host_stack[0].src_len = strlen(copyJob.copy_src_path);
      copyJob.host_stack[0].dst_len = strlen(copyJob.copy_dst_path);
      copyJob.depth = 1u;
      return true;
    }

    copyJob.current_file_src_parent_len = strlen(copyJob.copy_src_path);
    copyJob.current_file_dst_parent_len = strlen(copyJob.copy_dst_path);
    return copy_open_current_host_file(copyJob.info.bytes_total);
  }

  FRESULT fr = FR_OK;

  if (!copy_copy_string(copyJob.copy_src_path, sizeof(copyJob.copy_src_path),
                        copyJob.source_path) ||
      !copy_copy_string(copyJob.copy_dst_path, sizeof(copyJob.copy_dst_path),
                        copyJob.dest_path)) {
    copy_fail(FR_INVALID_NAME, "path too long");
    return false;
  }

  copyJob.depth = 0u;
  copyJob.copy_initialized = true;

  if (copyJob.info.src_is_dir) {
    fr = f_mkdir(copyJob.copy_dst_path);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot create destination folder");
      return false;
    }

    copyJob.info.dirs_done = 1u;
    copy_set_current_paths(copyJob.info.src_path, copyJob.info.dst_path);

    fr = f_opendir(&copyJob.host_stack[0].dir, copyJob.copy_src_path);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot open source folder");
      return false;
    }

    copyJob.host_stack[0].src_len = strlen(copyJob.copy_src_path);
    copyJob.host_stack[0].dst_len = strlen(copyJob.copy_dst_path);
    copyJob.depth = 1u;
    return true;
  }

  copyJob.current_file_src_parent_len = strlen(copyJob.copy_src_path);
  copyJob.current_file_dst_parent_len = strlen(copyJob.copy_dst_path);
  return copy_open_current_host_file(copyJob.info.bytes_total);
}

static bool copy_begin_copy_image(void) {
  FRESULT fr = FR_OK;

  if (!copy_copy_string(copyJob.copy_src_path, sizeof(copyJob.copy_src_path),
                        copyJob.source_path) ||
      !copy_copy_string(copyJob.copy_dst_path, sizeof(copyJob.copy_dst_path),
                        copyJob.dest_path)) {
    copy_fail(FR_INVALID_NAME, "path too long");
    return false;
  }

  copyJob.depth = 0u;
  copyJob.copy_initialized = true;

  if (copyJob.info.src_is_dir) {
    fr = f_mkdir(copyJob.copy_dst_path);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot create destination folder");
      return false;
    }

    copyJob.info.dirs_done = 1u;
    copy_set_current_image_paths(copyJob.source_path, copyJob.info.dst_path);

    fr = stfs_opendir(&copyJob.image_fs, copyJob.copy_src_path,
                      &copyJob.image_stack[0].dir);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot open image folder");
      return false;
    }

    copyJob.image_stack[0].src_len = strlen(copyJob.copy_src_path);
    copyJob.image_stack[0].dst_len = strlen(copyJob.copy_dst_path);
    copyJob.depth = 1u;
    return true;
  }

  copyJob.current_file_src_parent_len = strlen(copyJob.copy_src_path);
  copyJob.current_file_dst_parent_len = strlen(copyJob.copy_dst_path);
  return copy_open_current_image_file(copyJob.info.bytes_total);
}

static bool copy_process_scan_host(void) {
  if (!copyJob.scan_initialized && !copy_begin_scan_host()) return false;
  if (!copyJob.entry_info) {
    copy_fail(FR_NOT_ENOUGH_CORE, "copy state not initialized");
    return false;
  }

  for (int step = 0; step < COPY_POLL_STEPS; step++) {
    copy_host_stack_frame_t *frame = NULL;
    FILINFO *fno = NULL;
    FRESULT fr = FR_OK;

    if (copyJob.info.cancel_requested) {
      copy_cancel();
      return false;
    }

    if (copyJob.depth == 0u) {
      copyJob.scan_initialized = false;
      copyJob.info.status = COPY_STATUS_COPYING;
      return true;
    }

    frame = &copyJob.host_stack[copyJob.depth - 1u];
    fno = copyJob.entry_info;
    fr = f_readdir(&frame->dir, fno);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot scan source folder");
      return false;
    }

    if (fno->fname[0] == '\0') {
      (void)f_closedir(&frame->dir);
      copyJob.depth--;
      if (copyJob.depth > 0u) {
        copy_restore_path(copyJob.scan_path,
                          copyJob.host_stack[copyJob.depth - 1u].src_len);
      }
      continue;
    }

    if (fno->fattrib & AM_DIR) {
      size_t child_len = 0u;

      if (copyJob.depth >= COPY_MAX_DIR_DEPTH) {
        copy_fail(FR_DENIED, "directory nesting too deep");
        return false;
      }

      if (!copy_append_name(copyJob.scan_path, sizeof(copyJob.scan_path),
                            fno->fname, &child_len)) {
        copy_fail(FR_INVALID_NAME, "path too long");
        return false;
      }

      copyJob.info.dirs_total++;
      (void)copy_copy_string(copyJob.info.current_path,
                             sizeof(copyJob.info.current_path),
                             copyJob.scan_path);

      fr = f_opendir(&copyJob.host_stack[copyJob.depth].dir, copyJob.scan_path);
      if (fr != FR_OK) {
        copy_fail(fr, "cannot open nested folder");
        return false;
      }

      copyJob.host_stack[copyJob.depth].src_len = child_len;
      copyJob.depth++;
      continue;
    }

    copyJob.info.files_total++;
    copyJob.info.bytes_total += (uint64_t)fno->fsize;
    {
      size_t file_len = 0u;
      if (!copy_append_name(copyJob.scan_path, sizeof(copyJob.scan_path),
                            fno->fname, &file_len)) {
        copy_fail(FR_INVALID_NAME, "path too long");
        return false;
      }
      (void)copy_copy_string(copyJob.info.current_path,
                             sizeof(copyJob.info.current_path),
                             copyJob.scan_path);
      copy_restore_path(copyJob.scan_path, frame->src_len);
    }
  }

  return true;
}

static bool copy_process_scan_image(void) {
  if (!copyJob.scan_initialized && !copy_begin_scan_image()) return false;
  if (!copyJob.image_entry_info) {
    copy_fail(FR_NOT_ENOUGH_CORE, "copy state not initialized");
    return false;
  }

  for (int step = 0; step < COPY_POLL_STEPS; step++) {
    copy_image_stack_frame_t *frame = NULL;
    stfs_dirent_t *entry = NULL;
    FRESULT fr = FR_OK;

    if (copyJob.info.cancel_requested) {
      copy_cancel();
      return false;
    }

    if (copyJob.depth == 0u) {
      copyJob.scan_initialized = false;
      copyJob.info.status = COPY_STATUS_COPYING;
      return true;
    }

    frame = &copyJob.image_stack[copyJob.depth - 1u];
    entry = copyJob.image_entry_info;
    fr = stfs_readdir(&frame->dir, entry);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot scan image folder");
      return false;
    }

    if (entry->name[0] == '\0') {
      stfs_closedir(&frame->dir);
      copyJob.depth--;
      if (copyJob.depth > 0u) {
        copy_restore_path(copyJob.scan_path,
                          copyJob.image_stack[copyJob.depth - 1u].src_len);
      }
      continue;
    }

    if (entry->attr & AM_DIR) {
      size_t child_len = 0u;

      if (copyJob.depth >= COPY_MAX_DIR_DEPTH) {
        copy_fail(FR_DENIED, "directory nesting too deep");
        return false;
      }

      if (!copy_append_name(copyJob.scan_path, sizeof(copyJob.scan_path),
                            entry->name, &child_len)) {
        copy_fail(FR_INVALID_NAME, "path too long");
        return false;
      }

      copyJob.info.dirs_total++;
      (void)copy_set_current_image_paths(copyJob.scan_path, copyJob.info.dst_path);

      fr = stfs_opendir(&copyJob.image_fs, copyJob.scan_path,
                        &copyJob.image_stack[copyJob.depth].dir);
      if (fr != FR_OK) {
        copy_fail(fr, "cannot open nested image folder");
        return false;
      }

      copyJob.image_stack[copyJob.depth].src_len = child_len;
      copyJob.depth++;
      continue;
    }

    copyJob.info.files_total++;
    copyJob.info.bytes_total += (uint64_t)entry->size;
    {
      size_t file_len = 0u;
      if (!copy_append_name(copyJob.scan_path, sizeof(copyJob.scan_path),
                            entry->name, &file_len)) {
        copy_fail(FR_INVALID_NAME, "path too long");
        return false;
      }
      (void)copy_set_current_image_paths(copyJob.scan_path, copyJob.info.dst_path);
      copy_restore_path(copyJob.scan_path, frame->src_len);
    }
  }

  return true;
}

static bool copy_process_host_file_chunk(void) {
  UINT bytes_read = 0u;
  UINT bytes_written = 0u;
  FRESULT fr =
      f_read(&copyJob.src_file, copyJob.buffer, COPY_CHUNK_SIZE, &bytes_read);
  if (fr != FR_OK) {
    copy_fail(fr, "cannot read source file");
    return false;
  }

  if (bytes_read == 0u) {
    if (copy_is_image_dest()) {
      fr = stfs_close_write_file(&copyJob.image_dst_file, true);
      copyJob.image_dst_file_open = false;
    } else {
      fr = f_sync(&copyJob.dst_file);
      if (fr == FR_OK) {
        fr = f_close(&copyJob.dst_file);
      }
      copyJob.dst_file_open = false;
    }
    if (copyJob.src_file_open) {
      (void)f_close(&copyJob.src_file);
      copyJob.src_file_open = false;
    }
    if (fr != FR_OK) {
      copy_fail(fr, "cannot flush destination file");
      return false;
    }

    copyJob.file_copy_active = false;
    copyJob.info.current_file_size = 0u;
    copyJob.info.current_file_done = 0u;
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

  if (copy_is_image_dest()) {
    fr = stfs_write_file(&copyJob.image_dst_file, copyJob.buffer, bytes_read,
                         &bytes_written);
  } else {
    fr = f_write(&copyJob.dst_file, copyJob.buffer, bytes_read, &bytes_written);
  }
  if (fr != FR_OK || bytes_written != bytes_read) {
    copy_fail(fr == FR_OK ? FR_DISK_ERR : fr, "cannot write destination file");
    return false;
  }

  copyJob.info.bytes_done += (uint64_t)bytes_written;
  copyJob.info.current_file_done += (uint64_t)bytes_written;
  return true;
}

static bool copy_process_image_file_chunk(void) {
  UINT bytes_read = 0u;
  UINT bytes_written = 0u;
  FRESULT fr = stfs_read_file(&copyJob.image_src_file, copyJob.buffer,
                              COPY_CHUNK_SIZE, &bytes_read);
  if (fr != FR_OK) {
    copy_fail(fr, "cannot read image file");
    return false;
  }

  if (bytes_read == 0u) {
    fr = f_sync(&copyJob.dst_file);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot flush destination file");
      return false;
    }
    if (copyJob.image_src_file_open) {
      stfs_close_file(&copyJob.image_src_file);
      copyJob.image_src_file_open = false;
    }
    if (copyJob.dst_file_open) {
      (void)f_close(&copyJob.dst_file);
      copyJob.dst_file_open = false;
    }
    copyJob.file_copy_active = false;
    copyJob.info.current_file_size = 0u;
    copyJob.info.current_file_done = 0u;
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

  fr = f_write(&copyJob.dst_file, copyJob.buffer, bytes_read, &bytes_written);
  if (fr != FR_OK || bytes_written != bytes_read) {
    copy_fail(fr == FR_OK ? FR_DISK_ERR : fr, "cannot write destination file");
    return false;
  }

  copyJob.info.bytes_done += (uint64_t)bytes_written;
  copyJob.info.current_file_done += (uint64_t)bytes_written;
  return true;
}

static bool copy_process_copy_host(void) {
  if (!copyJob.copy_initialized && !copy_begin_copy_host()) return false;
  if (!copyJob.entry_info) {
    copy_fail(FR_NOT_ENOUGH_CORE, "copy state not initialized");
    return false;
  }

  for (int step = 0; step < COPY_POLL_STEPS; step++) {
    copy_host_stack_frame_t *frame = NULL;
    FILINFO *fno = NULL;
    FRESULT fr = FR_OK;

    if (copyJob.info.cancel_requested) {
      copy_cancel();
      return false;
    }

    if (copyJob.file_copy_active) {
      if (!copy_process_host_file_chunk()) return false;
      if (copyJob.info.status == COPY_STATUS_COMPLETED) return true;
      continue;
    }

    if (!copyJob.info.src_is_dir) {
      if (!copy_open_current_host_file(copyJob.info.bytes_total)) return false;
      continue;
    }

    if (copyJob.depth == 0u) return copy_complete_success();

    frame = &copyJob.host_stack[copyJob.depth - 1u];
    fno = copyJob.entry_info;
    fr = f_readdir(&frame->dir, fno);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot read source folder");
      return false;
    }

    if (fno->fname[0] == '\0') {
      (void)f_closedir(&frame->dir);
      copyJob.depth--;
      if (copyJob.depth > 0u) {
        copy_restore_path(copyJob.copy_src_path,
                          copyJob.host_stack[copyJob.depth - 1u].src_len);
        copy_restore_path(copyJob.copy_dst_path,
                          copyJob.host_stack[copyJob.depth - 1u].dst_len);
      }
      continue;
    }

    {
      size_t child_src_len = 0u;
      size_t child_dst_len = 0u;
      char resolved_name[STFS_MAX_NAME_LEN];

      if (!copy_append_name(copyJob.copy_src_path, sizeof(copyJob.copy_src_path),
                            fno->fname, &child_src_len)) {
        copy_fail(FR_INVALID_NAME, "path too long");
        return false;
      }

      if (copy_is_image_dest()) {
        FRESULT name_fr = stfs_resolve_sfn_name(&copyJob.image_fs,
                                                copyJob.copy_dst_path, fno->fname,
                                                resolved_name,
                                                sizeof(resolved_name), NULL);
        if (name_fr != FR_OK ||
            !copy_append_name(copyJob.copy_dst_path,
                              sizeof(copyJob.copy_dst_path), resolved_name,
                              &child_dst_len)) {
          copy_fail(name_fr == FR_OK ? FR_INVALID_NAME : name_fr,
                    "cannot resolve image destination name");
          return false;
        }
      } else if (!copy_append_name(copyJob.copy_dst_path,
                                   sizeof(copyJob.copy_dst_path), fno->fname,
                                   &child_dst_len)) {
        copy_fail(FR_INVALID_NAME, "path too long");
        return false;
      }

      if (fno->fattrib & AM_DIR) {
        if (copyJob.depth >= COPY_MAX_DIR_DEPTH) {
          copy_fail(FR_DENIED, "directory nesting too deep");
          return false;
        }

        if (copy_is_image_dest()) {
          char parent_path[COPY_MAX_PATH_LEN];
          char name[STFS_MAX_NAME_LEN];

          if (!copy_split_parent_child(copyJob.copy_dst_path, parent_path,
                                       sizeof(parent_path), name,
                                       sizeof(name))) {
            copy_fail(FR_INVALID_NAME, "invalid image destination path");
            return false;
          }
          copy_set_current_host_to_image_paths(copyJob.copy_src_path,
                                               copyJob.copy_dst_path);
          fr = stfs_mkdir(&copyJob.image_fs, parent_path, name,
                          (BYTE)(fno->fattrib & ~AM_DIR), fno->fdate,
                          fno->ftime);
        } else {
          copy_set_current_paths(copyJob.copy_src_path, copyJob.copy_dst_path);
          fr = f_mkdir(copyJob.copy_dst_path);
        }
        if (fr != FR_OK) {
          copy_fail(fr, "cannot create nested folder");
          return false;
        }

        copyJob.info.dirs_done++;
        fr = f_opendir(&copyJob.host_stack[copyJob.depth].dir,
                       copyJob.copy_src_path);
        if (fr != FR_OK) {
          copy_fail(fr, "cannot open nested source folder");
          return false;
        }

        copyJob.host_stack[copyJob.depth].src_len = child_src_len;
        copyJob.host_stack[copyJob.depth].dst_len = child_dst_len;
        copyJob.depth++;
        continue;
      }

      copyJob.current_file_src_parent_len = frame->src_len;
      copyJob.current_file_dst_parent_len = frame->dst_len;
      if (!copy_open_current_host_file((uint64_t)fno->fsize)) return false;
    }
  }

  return true;
}

static bool copy_process_copy_image(void) {
  if (!copyJob.copy_initialized && !copy_begin_copy_image()) return false;
  if (!copyJob.image_entry_info) {
    copy_fail(FR_NOT_ENOUGH_CORE, "copy state not initialized");
    return false;
  }

  for (int step = 0; step < COPY_POLL_STEPS; step++) {
    copy_image_stack_frame_t *frame = NULL;
    stfs_dirent_t *entry = NULL;
    FRESULT fr = FR_OK;

    if (copyJob.info.cancel_requested) {
      copy_cancel();
      return false;
    }

    if (copyJob.file_copy_active) {
      if (!copy_process_image_file_chunk()) return false;
      if (copyJob.info.status == COPY_STATUS_COMPLETED) return true;
      continue;
    }

    if (!copyJob.info.src_is_dir) {
      if (!copy_open_current_image_file(copyJob.info.bytes_total)) return false;
      continue;
    }

    if (copyJob.depth == 0u) return copy_complete_success();

    frame = &copyJob.image_stack[copyJob.depth - 1u];
    entry = copyJob.image_entry_info;
    fr = stfs_readdir(&frame->dir, entry);
    if (fr != FR_OK) {
      copy_fail(fr, "cannot read image folder");
      return false;
    }

    if (entry->name[0] == '\0') {
      stfs_closedir(&frame->dir);
      copyJob.depth--;
      if (copyJob.depth > 0u) {
        copy_restore_path(copyJob.copy_src_path,
                          copyJob.image_stack[copyJob.depth - 1u].src_len);
        copy_restore_path(copyJob.copy_dst_path,
                          copyJob.image_stack[copyJob.depth - 1u].dst_len);
      }
      continue;
    }

    {
      size_t child_src_len = 0u;
      size_t child_dst_len = 0u;

      if (!copy_append_name(copyJob.copy_src_path, sizeof(copyJob.copy_src_path),
                            entry->name, &child_src_len) ||
          !copy_append_name(copyJob.copy_dst_path, sizeof(copyJob.copy_dst_path),
                            entry->name, &child_dst_len)) {
        copy_fail(FR_INVALID_NAME, "path too long");
        return false;
      }

      if (entry->attr & AM_DIR) {
        if (copyJob.depth >= COPY_MAX_DIR_DEPTH) {
          copy_fail(FR_DENIED, "directory nesting too deep");
          return false;
        }

        copy_set_current_image_paths(copyJob.copy_src_path, copyJob.copy_dst_path);
        fr = f_mkdir(copyJob.copy_dst_path);
        if (fr != FR_OK) {
          copy_fail(fr, "cannot create nested folder");
          return false;
        }

        copyJob.info.dirs_done++;
        fr = stfs_opendir(&copyJob.image_fs, copyJob.copy_src_path,
                          &copyJob.image_stack[copyJob.depth].dir);
        if (fr != FR_OK) {
          copy_fail(fr, "cannot open nested image folder");
          return false;
        }

        copyJob.image_stack[copyJob.depth].src_len = child_src_len;
        copyJob.image_stack[copyJob.depth].dst_len = child_dst_len;
        copyJob.depth++;
        continue;
      }

      copyJob.current_file_src_parent_len = frame->src_len;
      copyJob.current_file_dst_parent_len = frame->dst_len;
      if (!copy_open_current_image_file((uint64_t)entry->size)) return false;
    }
  }

  return true;
}

static void copy_init_info(copy_operation_t operation, bool src_is_dir,
                           uint64_t file_size) {
  copyJob.info.operation = operation;
  copyJob.info.src_is_dir = src_is_dir;
  copyJob.info.status = src_is_dir ? COPY_STATUS_SCANNING : COPY_STATUS_COPYING;
  copyJob.info.last_error = FR_OK;
  copyJob.info.error_message[0] = '\0';
  copyJob.info.bytes_total = src_is_dir ? 0u : file_size;
  copyJob.info.files_total = src_is_dir ? 0u : 1u;
  copyJob.info.dirs_total = 0u;
  copyJob.info.files_done = 0u;
  copyJob.info.dirs_done = 0u;
  copyJob.info.bytes_done = 0u;
  copyJob.info.current_file_size = 0u;
  copyJob.info.current_file_done = 0u;
  copyJob.info.cancel_requested = false;
  copy_clear_current_paths();
}

static bool copy_start_host_internal(const char *src_path, const char *dst_path,
                                     copy_operation_t operation) {
  BYTE src_fattrib = 0u;
  FSIZE_t src_fsize = 0u;
  FRESULT fr = FR_OK;

  copy_clear_start_error();

  if (!src_path || !dst_path || src_path[0] == '\0' || dst_path[0] == '\0') {
    copy_set_start_error(FR_INVALID_PARAMETER, "invalid source or destination");
    return false;
  }
  if (copy_is_active()) {
    copy_set_start_error(FR_DENIED, "another copy or move is already active");
    return false;
  }

  copy_reset_job();

  if (!copy_copy_string(copyJob.source_path, sizeof(copyJob.source_path),
                        src_path) ||
      !copy_copy_string(copyJob.info.src_path, sizeof(copyJob.info.src_path),
                        src_path) ||
      !copy_copy_string(copyJob.dest_path, sizeof(copyJob.dest_path), dst_path) ||
      !copy_copy_string(copyJob.info.dst_path, sizeof(copyJob.info.dst_path),
                        dst_path)) {
    copy_set_start_error(FR_INVALID_NAME, "source or destination path is too long");
    copy_reset_job();
    return false;
  }

  copyJob.source = COPY_SOURCE_HOST;
  copyJob.destination = COPY_DEST_HOST;
  copyJob.host_stack = calloc(COPY_MAX_DIR_DEPTH, sizeof(*copyJob.host_stack));
  copyJob.buffer = malloc(COPY_CHUNK_SIZE);
  copyJob.entry_info = calloc(1, sizeof(*copyJob.entry_info));
  if (!copyJob.host_stack || !copyJob.buffer || !copyJob.entry_info) {
    copy_set_start_error(FR_NOT_ENOUGH_CORE, "out of memory");
    copy_reset_job();
    return false;
  }

  fr = f_stat(copyJob.source_path, copyJob.entry_info);
  if (fr != FR_OK) {
    copy_set_start_error(fr, "source not found");
    copy_reset_job();
    return false;
  }
  src_fattrib = copyJob.entry_info->fattrib;
  src_fsize = copyJob.entry_info->fsize;

  fr = f_stat(copyJob.dest_path, copyJob.entry_info);
  if (fr == FR_OK || (fr != FR_NO_FILE && fr != FR_NO_PATH)) {
    copy_set_start_error(fr == FR_OK ? FR_EXIST : fr,
                         fr == FR_OK ? "destination already exists"
                                     : "destination check failed");
    copy_reset_job();
    return false;
  }

  copy_init_info(operation, (src_fattrib & AM_DIR) != 0u, (uint64_t)src_fsize);
  return true;
}

bool copy_start(const char *src_path, const char *dst_path) {
  return copy_start_host_internal(src_path, dst_path, COPY_OPERATION_COPY);
}

bool copy_start_move(const char *src_path, const char *dst_path) {
  return copy_start_host_internal(src_path, dst_path, COPY_OPERATION_MOVE);
}

bool copy_start_to_image(const char *src_path, const char *image_file_path,
                         const char *image_dst_dir) {
  FRESULT fr = FR_OK;
  char resolved_name[STFS_MAX_NAME_LEN];
  stfs_dirent_t image_dir_info = {0};
  size_t ignored_len = 0u;

  copy_clear_start_error();

  if (!src_path || !image_file_path || !image_dst_dir || src_path[0] == '\0' ||
      image_file_path[0] == '\0' || image_dst_dir[0] == '\0') {
    copy_set_start_error(FR_INVALID_PARAMETER, "invalid source or image path");
    return false;
  }
  if (copy_is_active()) {
    copy_set_start_error(FR_DENIED, "another copy or move is already active");
    return false;
  }

  copy_reset_job();

  if (!copy_copy_string(copyJob.source_path, sizeof(copyJob.source_path),
                        src_path) ||
      !copy_copy_string(copyJob.info.src_path, sizeof(copyJob.info.src_path),
                        src_path) ||
      !copy_copy_string(copyJob.image_file_path,
                        sizeof(copyJob.image_file_path), image_file_path)) {
    copy_set_start_error(FR_INVALID_NAME, "source or image path is too long");
    copy_reset_job();
    return false;
  }

  copyJob.source = COPY_SOURCE_HOST;
  copyJob.destination = COPY_DEST_IMAGE;
  copyJob.host_stack = calloc(COPY_MAX_DIR_DEPTH, sizeof(*copyJob.host_stack));
  copyJob.buffer = malloc(COPY_CHUNK_SIZE);
  copyJob.entry_info = calloc(1, sizeof(*copyJob.entry_info));
  if (!copyJob.host_stack || !copyJob.buffer || !copyJob.entry_info) {
    copy_set_start_error(FR_NOT_ENOUGH_CORE, "out of memory");
    copy_reset_job();
    return false;
  }

  fr = f_stat(copyJob.source_path, copyJob.entry_info);
  if (fr != FR_OK) {
    copy_set_start_error(fr, "source not found");
    copy_reset_job();
    return false;
  }

  fr = stfs_open_rw(&copyJob.image_fs, copyJob.image_file_path);
  if (fr != FR_OK) {
    copy_set_start_error(fr, "image is not writable or not a FAT floppy");
    copy_reset_job();
    return false;
  }
  copyJob.image_fs_open = true;

  fr = stfs_stat(&copyJob.image_fs, image_dst_dir, &image_dir_info);
  if (fr != FR_OK ||
      !(strcmp(image_dst_dir, "/") == 0 ||
        (image_dir_info.attr & AM_DIR) != 0u)) {
    copy_set_start_error(fr == FR_OK ? FR_NO_PATH : fr,
                         "destination folder not found inside image");
    copy_reset_job();
    return false;
  }

  fr = stfs_resolve_sfn_name(&copyJob.image_fs, image_dst_dir,
                             copy_basename(copyJob.source_path), resolved_name,
                             sizeof(resolved_name), NULL);
  if (fr != FR_OK) {
    copy_set_start_error(fr, fr == FR_EXIST
                                 ? "destination already exists inside image"
                                 : fr == FR_INVALID_NAME
                                       ? "source name cannot be converted to 8.3"
                                       : "could not resolve destination name inside image");
    copy_reset_job();
    return false;
  }
  if (
      !copy_copy_string(copyJob.dest_path, sizeof(copyJob.dest_path),
                        image_dst_dir) ||
      !copy_append_name(copyJob.dest_path, sizeof(copyJob.dest_path),
                        resolved_name, &ignored_len) ||
      !copy_format_image_display_path(copyJob.info.dst_path,
                                      sizeof(copyJob.info.dst_path),
                                      image_file_path, copyJob.dest_path)) {
    copy_set_start_error(FR_INVALID_NAME,
                         "destination path inside image is too long");
    copy_reset_job();
    return false;
  }

  copy_init_info(COPY_OPERATION_COPY,
                 (copyJob.entry_info->fattrib & AM_DIR) != 0u,
                 (uint64_t)copyJob.entry_info->fsize);
  return true;
}

bool copy_start_from_image(const char *image_file_path,
                           const char *image_src_path, const char *dst_path) {
  FRESULT fr = FR_OK;

  copy_clear_start_error();

  if (!image_file_path || !image_src_path || !dst_path ||
      image_file_path[0] == '\0' || image_src_path[0] == '\0' ||
      dst_path[0] == '\0') {
    copy_set_start_error(FR_INVALID_PARAMETER, "invalid image or destination path");
    return false;
  }
  if (copy_is_active()) {
    copy_set_start_error(FR_DENIED, "another copy or move is already active");
    return false;
  }

  copy_reset_job();

  if (!copy_copy_string(copyJob.source_path, sizeof(copyJob.source_path),
                        image_src_path) ||
      !copy_copy_string(copyJob.image_file_path,
                        sizeof(copyJob.image_file_path), image_file_path) ||
      !copy_copy_string(copyJob.dest_path, sizeof(copyJob.dest_path), dst_path) ||
      !copy_copy_string(copyJob.info.dst_path, sizeof(copyJob.info.dst_path),
                        dst_path) ||
      !copy_format_image_display_path(copyJob.info.src_path,
                                      sizeof(copyJob.info.src_path),
                                      image_file_path, image_src_path)) {
    copy_set_start_error(FR_INVALID_NAME, "image or destination path is too long");
    copy_reset_job();
    return false;
  }

  copyJob.source = COPY_SOURCE_IMAGE;
  copyJob.destination = COPY_DEST_HOST;
  copyJob.image_stack =
      calloc(COPY_MAX_DIR_DEPTH, sizeof(*copyJob.image_stack));
  copyJob.buffer = malloc(COPY_CHUNK_SIZE);
  copyJob.entry_info = calloc(1, sizeof(*copyJob.entry_info));
  copyJob.image_entry_info = calloc(1, sizeof(*copyJob.image_entry_info));
  if (!copyJob.image_stack || !copyJob.buffer || !copyJob.entry_info ||
      !copyJob.image_entry_info) {
    copy_set_start_error(FR_NOT_ENOUGH_CORE, "out of memory");
    copy_reset_job();
    return false;
  }

  fr = stfs_open(&copyJob.image_fs, copyJob.image_file_path);
  if (fr != FR_OK) {
    copy_set_start_error(fr, "image is not a browsable FAT floppy");
    copy_reset_job();
    return false;
  }
  copyJob.image_fs_open = true;

  fr = stfs_stat(&copyJob.image_fs, copyJob.source_path, copyJob.image_entry_info);
  if (fr != FR_OK) {
    copy_set_start_error(fr, "source path not found inside image");
    copy_reset_job();
    return false;
  }

  fr = f_stat(copyJob.dest_path, copyJob.entry_info);
  if (fr == FR_OK || (fr != FR_NO_FILE && fr != FR_NO_PATH)) {
    copy_set_start_error(fr == FR_OK ? FR_EXIST : fr,
                         fr == FR_OK ? "destination already exists"
                                     : "destination check failed");
    copy_reset_job();
    return false;
  }

  copy_init_info(COPY_OPERATION_COPY,
                 (copyJob.image_entry_info->attr & AM_DIR) != 0u,
                 (uint64_t)copyJob.image_entry_info->size);
  return true;
}

bool copy_reset_status(void) {
  if (copy_is_active()) return false;
  copy_reset_job();
  return true;
}

FRESULT copy_get_last_start_error(void) { return copyLastStartError; }

const char *copy_get_last_start_error_message(void) {
  return copyLastStartErrorMessage;
}

void copy_poll(void) {
  switch (copyJob.info.status) {
    case COPY_STATUS_SCANNING:
      (void)(copy_is_image_source() ? copy_process_scan_image()
                                    : copy_process_scan_host());
      break;
    case COPY_STATUS_COPYING:
      (void)(copy_is_image_source() ? copy_process_copy_image()
                                    : copy_process_copy_host());
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
