/**
 * File: mngr_httpd.h
 * Author: Diego Parrilla Santamaría
 * Date: December 2025
 * Copyright: 2024-2025 - GOODDATA LABS SL
 * Description: Header file for the manager mode httpd server.
 */

#ifndef MNGR_HTTPD_H
#define MNGR_HTTPD_H

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "constants.h"
#include "debug.h"
#include "download.h"
#include "include/aconfig.h"
#include "lwip/apps/fs.h"
#include "lwip/apps/httpd.h"
#include "mbedtls/base64.h"
#include "mngr.h"
#include "network.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "settings/settings.h"

#ifndef MAX_JSON_PAYLOAD_SIZE
#define MAX_JSON_PAYLOAD_SIZE 4096
#endif

#ifndef MNGR_HTTPD_ALLOWED_ROOT
#define MNGR_HTTPD_ALLOWED_ROOT "/"
#endif

#ifndef MNGR_HTTPD_MAX_PATH_LEN
#define MNGR_HTTPD_MAX_PATH_LEN 512
#endif
#ifndef MNGR_HTTPD_MAX_FOLDER_LEN
#define MNGR_HTTPD_MAX_FOLDER_LEN 256
#endif
#ifndef MNGR_HTTPD_MAX_NAME_LEN
#define MNGR_HTTPD_MAX_NAME_LEN 128
#endif
#ifndef MNGR_HTTPD_MAX_TOKEN_LEN
#define MNGR_HTTPD_MAX_TOKEN_LEN 32
#endif
#ifndef MNGR_HTTPD_ERROR_URL_LEN
#define MNGR_HTTPD_ERROR_URL_LEN 128
#endif
#ifndef MNGR_HTTPD_RESPONSE_MSG_LEN
#define MNGR_HTTPD_RESPONSE_MSG_LEN 128
#endif
#ifndef MNGR_HTTPD_SSI_JSON_CHUNK_SIZE
#define MNGR_HTTPD_SSI_JSON_CHUNK_SIZE 128
#endif

#ifndef HTTPD_JSON_STATE_SLOTS
#define HTTPD_JSON_STATE_SLOTS 4
#endif

#ifndef UPLOAD_CHUNK_SIZE
#define UPLOAD_CHUNK_SIZE 4096
#endif

// Default upload chunk method: "GET" (base64) or "POST" (binary)
#ifndef UPLOAD_CHUNK_METHOD
#define UPLOAD_CHUNK_METHOD "POST"
#endif

// Default download chunk size (raw bytes) to fit JSON buffer after base64.
#ifndef DOWNLOAD_CHUNK_SIZE
#define DOWNLOAD_CHUNK_SIZE 2048
#endif

typedef enum {
  MNGR_HTTPD_RESPONSE_OK = 200,
  MNGR_HTTPD_RESPONSE_BAD_REQUEST = 400,
  MNGR_HTTPD_RESPONSE_NOT_FOUND = 404,
  MNGR_HTTPD_RESPONSE_INTERNAL_SERVER_ERROR = 500
} mngr_httpd_response_status_t;

#if defined(LWIP_HTTPD_FILE_STATE) && LWIP_HTTPD_FILE_STATE
typedef struct {
  bool in_use;
  char json_snapshot[MAX_JSON_PAYLOAD_SIZE];
} httpd_json_state_t;
#endif

void mngr_httpd_start(int sdcard_err);

#endif  // MNGR_HTTPD_H
