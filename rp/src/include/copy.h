/**
 * File: copy.h
 * Author: Diego Parrilla Santamaría
 * Date: March 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Background file and folder copy job support.
 */

#ifndef COPY_H
#define COPY_H

#include <stdbool.h>
#include <stdint.h>

#include "ff.h"

#ifndef COPY_MAX_PATH_LEN
#define COPY_MAX_PATH_LEN 448
#endif

#ifndef COPY_MAX_DIR_DEPTH
#define COPY_MAX_DIR_DEPTH 16
#endif

#ifndef COPY_CHUNK_SIZE
#define COPY_CHUNK_SIZE 4096
#endif

#ifndef COPY_ERROR_MSG_LEN
#define COPY_ERROR_MSG_LEN 128
#endif

typedef enum {
  COPY_STATUS_IDLE = 0,
  COPY_STATUS_SCANNING,
  COPY_STATUS_COPYING,
  COPY_STATUS_COMPLETED,
  COPY_STATUS_FAILED,
  COPY_STATUS_CANCELLED
} copy_status_t;

typedef enum {
  COPY_OPERATION_COPY = 0,
  COPY_OPERATION_MOVE
} copy_operation_t;

typedef struct {
  copy_status_t status;
  copy_operation_t operation;
  bool src_is_dir;
  bool cancel_requested;
  char src_path[COPY_MAX_PATH_LEN];
  char dst_path[COPY_MAX_PATH_LEN];
  char current_path[COPY_MAX_PATH_LEN];
  char current_dst_path[COPY_MAX_PATH_LEN];
  uint32_t files_total;
  uint32_t files_done;
  uint32_t dirs_total;
  uint32_t dirs_done;
  uint64_t bytes_total;
  uint64_t bytes_done;
  uint64_t current_file_size;
  uint64_t current_file_done;
  FRESULT last_error;
  char error_message[COPY_ERROR_MSG_LEN];
} copy_info_t;

bool copy_start(const char *src_path, const char *dst_path);
bool copy_start_move(const char *src_path, const char *dst_path);
bool copy_reset_status(void);
void copy_poll(void);
void copy_request_cancel(void);
bool copy_is_active(void);
copy_status_t copy_get_status(void);
void copy_get_info(copy_info_t *info);
const char *copy_status_to_string(copy_status_t status);
const char *copy_operation_to_string(copy_operation_t operation);

#endif  // COPY_H
