/**
 * File: mngr_httpd.c
 * Author: Diego Parrilla Santamaría
 * Date: December 2024
 * Copyright: 2024-2025 - GOODDATA LABS SL
 * Description: HTTPD server functions for manager httpd
 */

#include "mngr_httpd.h"
static mngr_httpd_response_status_t response_status = MNGR_HTTPD_RESPONSE_OK;
static char json_buff[MAX_JSON_PAYLOAD_SIZE] = {0};  // Buffer for JSON payload
static char httpd_response_message[MNGR_HTTPD_RESPONSE_MSG_LEN] = {0};
static int sdcard_status = SDCARD_INIT_ERROR;

// Per-connection/per-file state for lwIP httpd when serving /json.shtml.
// This avoids races where the global json buffer is overwritten while SSI
// multipart output is still in progress on another connection.
#if defined(LWIP_HTTPD_FILE_STATE) && LWIP_HTTPD_FILE_STATE
static httpd_json_state_t httpd_json_states[HTTPD_JSON_STATE_SLOTS] = {0};


void *fs_state_init(struct fs_file *file, const char *name) {
  LWIP_UNUSED_ARG(file);
  if (!name) return NULL;
  if (strcmp(name, "/json.shtml") != 0) return NULL;

  for (int i = 0; i < HTTPD_JSON_STATE_SLOTS; i++) {
    if (!httpd_json_states[i].in_use) {
      httpd_json_states[i].in_use = true;
      // Snapshot current response JSON so it stays stable during SSI multipart.
      (void)snprintf(httpd_json_states[i].json_snapshot,
                     sizeof(httpd_json_states[i].json_snapshot), "%s",
                     json_buff);
      return &httpd_json_states[i];
    }
  }
  // If no slots available, fall back to global buffer (may be corrupted).
  return NULL;
}

void fs_state_free(struct fs_file *file, void *state) {
  LWIP_UNUSED_ARG(file);
  if (!state) return;
  httpd_json_state_t *st = (httpd_json_state_t *)state;
  st->in_use = false;
  st->json_snapshot[0] = '\0';
}
#endif

static bool path_has_forbidden_chars(const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (s[i] == ':' || s[i] == '\\') return true;
  }
  return false;
}

static bool segment_is_forbidden(const char *seg, size_t len) {
  if (len == 0) return true;
  if (path_has_forbidden_chars(seg, len)) return true;
  // Reject any segment containing ".." anywhere.
  for (size_t i = 0; i + 1 < len; i++) {
    if (seg[i] == '.' && seg[i + 1] == '.') return true;
  }
  return false;
}

// Normalize a path to an absolute path, collapsing repeated slashes and
// validating segments. Returns false if invalid or output too small.
static bool normalize_and_validate_path(const char *in, char *out,
                                        size_t out_len) {
  if (!in || !out || out_len < 2) return false;

  char *tmp = NULL;
  bool ok = false;

  // Support in-place normalization safely
  // (normalize_and_validate_path(out,out)). join_root_folder_name() uses
  // in-place normalization today.
  if (in == out) {
    tmp = malloc(out_len);
    if (!tmp) goto cleanup;
    size_t in_len = strnlen(in, out_len);
    if (in_len >= out_len) goto cleanup;
    memcpy(tmp, in, in_len + 1);
    in = tmp;
  }

  // Treat empty as root.
  if (*in == '\0') {
    out[0] = '/';
    out[1] = '\0';
    ok = true;
    goto cleanup;
  }

  size_t o = 0;
  const char *p = in;

  // Force absolute.
  if (*p != '/') out[o++] = '/';

  bool prev_was_slash = (o > 0);
  const char *seg_start = NULL;
  size_t seg_len = 0;

  while (*p) {
    char ch = *p++;
    if (ch == '/') {
      if (seg_start) {
        if (segment_is_forbidden(seg_start, seg_len)) goto cleanup;
        seg_start = NULL;
        seg_len = 0;
      }
      if (!prev_was_slash) {
        if (o + 1 >= out_len) goto cleanup;
        out[o++] = '/';
        prev_was_slash = true;
      }
      continue;
    }

    if (!seg_start) {
      seg_start = p - 1;
      seg_len = 0;
    }
    seg_len++;
    if (path_has_forbidden_chars(&ch, 1)) goto cleanup;

    if (o + 1 >= out_len) goto cleanup;
    out[o++] = ch;
    prev_was_slash = false;
  }

  if (seg_start) {
    if (segment_is_forbidden(seg_start, seg_len)) goto cleanup;
  }

  // Trim trailing slash unless root.
  if (o > 1 && out[o - 1] == '/') o--;
  if (o >= out_len) goto cleanup;
  out[o] = '\0';

  // Ensure allowed root prefix.
  if (strncmp(out, MNGR_HTTPD_ALLOWED_ROOT, strlen(MNGR_HTTPD_ALLOWED_ROOT)) !=
      0) {
    goto cleanup;
  }
  ok = true;

cleanup:
  free(tmp);
  return ok;
}

static bool validate_filename_only(const char *name) {
  if (!name || *name == '\0') return false;
  // No path separators, no forbidden chars, no "..".
  if (strchr(name, '/') || strchr(name, '\\')) return false;
  if (strchr(name, ':')) return false;
  if (strstr(name, "..")) return false;
  return true;
}

static bool join_root_folder_name(const char *folder_abs, const char *name,
                                  char *out, size_t out_len) {
  if (!folder_abs || !name || !out) return false;
  if (!validate_filename_only(name)) return false;
  if (folder_abs[0] != '/') return false;
  size_t flen = strlen(folder_abs);
  if (flen == 0) return false;
  bool need_slash = folder_abs[flen - 1] != '/';
  int n =
      snprintf(out, out_len, "%s%s%s", folder_abs, need_slash ? "/" : "", name);
  if (n < 0 || (size_t)n >= out_len) return false;
  return normalize_and_validate_path(out, out, out_len);
}

// Decodes a URI-escaped string (percent-encoded) into its original value.
// E.g., "My%20SSID%21" -> "My SSID!"
// Returns true on success. Always null-terminates output.
// Output buffer must be at least outLen bytes.
// Returns false if input is NULL, output is NULL, or output buffer is too
// small.
static bool url_decode(const char *in, char *out, size_t outLen) {
  if (!in || !out || outLen == 0) return false;

  DPRINTF("Decoding '%s'. Length=%lu\n", in, (unsigned long)outLen);
  size_t o = 0;
  for (size_t i = 0; in[i] && o < outLen - 1; ++i) {
    if (in[i] == '%' && in[i + 1] && in[i + 2] &&
        isxdigit((unsigned char)in[i + 1]) &&
        isxdigit((unsigned char)in[i + 2])) {
      // Decode %xx
      char hi = in[i + 1], lo = in[i + 2];
      int high = (hi >= 'A' ? (toupper(hi) - 'A' + 10) : (hi - '0'));
      int low = (lo >= 'A' ? (toupper(lo) - 'A' + 10) : (lo - '0'));
      out[o++] = (char)((high << 4) | low);
      i += 2;
    } else if (in[i] == '+') {
      // Optionally decode + as space (common in x-www-form-urlencoded)
      out[o++] = ' ';
    } else {
      out[o++] = in[i];
    }
  }
  out[o] = '\0';
  DPRINTF("Decoded to '%s'. Size: %lu\n", out, (unsigned long)(o + 1));
  return true;
}

static bool json_appendf(char *buf, size_t buf_size, size_t *buf_len,
                         const char *fmt, ...) {
  if (!buf || !buf_len || *buf_len >= buf_size) return false;
  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(buf + *buf_len, buf_size - *buf_len, fmt, args);
  va_end(args);
  if (written < 0) return false;
  if ((size_t)written >= buf_size - *buf_len) {
    // vsnprintf truncated; ensure buffer is null-terminated.
    buf[buf_size - 1] = '\0';
    *buf_len = buf_size - 1;
    return false;
  }
  *buf_len += (size_t)written;
  return true;
}

/**
 * @brief Array of SSI tags for the HTTP server.
 *
 * This array contains the SSI tags used by the HTTP server to dynamically
 * insert content into web pages. Each tag corresponds to a specific piece of
 * information that can be updated or retrieved from the server.
 */
static const char *ssi_tags[] = {
    // Max size of SSI tag is 8 chars
    "HOMEPAGE",  // 0 - Redirect to the homepage
    "SDCARDST",  // 1 - SD card status
    "APPSFLDR",  // 2 - Apps folder
    "SSID",      // 3 - SSID
    "IPADDR",    // 4 - IP address
    "SDCARDB",   // 5 - SD card status true or false
    "APPSFLDB",  // 6 - Apps folder status true or false
    "JSONPLD",   // 7 - JSON payload
    "DWNLDSTS",  // 8 - Download status
    "TITLEHDR",  // 9 - Title header
    "WIFILST",   // 10 - WiFi list
    "WLSTSTP",   // 11 - WiFi list stop
    "RSPSTS",    // 12 - Response status
    "RSPMSG",    // 13 - Response message
    "BTFTR",     // 14 - Boot feature
    "SFCNFGRB",  // 15 - Safe Config Reboot
    "SDBDRTKB",  // 16 - SD Card Baud Rate
    "APPSURL",   // 17 - Apps Catalog URL
    "PLHLDR9",   // 18 - Placeholder 9
    "PLHLDR10",  // 19 - Placeholder 10
    "PLHLDR11",  // 20 - Placeholder 11
    "PLHLDR12",  // 21 - Placeholder 12
    "PLHLDR13",  // 22 - Placeholder 13
    "PLHLDR14",  // 23 - Placeholder 14
    "PLHLDR15",  // 24 - Placeholder 15
    "PLHLDR16",  // 25 - Placeholder 16
    "PLHLDR17",  // 26 - Placeholder 17
    "PLHLDR18",  // 27 - Placeholder 18
    "PLHLDR19",  // 28 - Placeholder 19
    "PLHLDR20",  // 29 - Placeholder 20
    "PLHLDR21",  // 30 - Placeholder 21
    "PLHLDR22",  // 31 - Placeholder 22
    "PLHLDR23",  // 32 - Placeholder 23
    "PLHLDR24",  // 33 - Placeholder 24
    "PLHLDR25",  // 34 - Placeholder 25
    "PLHLDR26",  // 35 - Placeholder 26
    "PLHLDR27",  // 36 - Placeholder 27
    "PLHLDR28",  // 37 - Placeholder 28
    "PLHLDR29",  // 38 - Placeholder 29
    "PLHLDR30",  // 39 - Placeholder 30
    "WDHCP",     // 40 - WiFi DHCP
    "WIP",       // 41 - WiFi IP
    "WNTMSK",    // 42 - WiFi Netmask
    "WGTWY",     // 43 - WiFi Gateway
    "WDNS",      // 44 - WiFi DNS
    "WCNTRY",    // 45 - WiFi Country
    "WHSTNM",    // 46 - WiFi Hostname
    "WPWR",      // 47 - WiFi Power
    "WRSS",      // 48 - WiFi RSSI
};

/**
 * @brief
 *
 * @param iIndex The index of the CGI handler.
 * @param iNumParams The number of parameters passed to the CGI handler.
 * @param pcParam An array of parameter names.
 * @param pcValue An array of parameter values.
 * @return The URL of the page to redirect to after the floppy disk image is
 * selected.
 */
static const char *cgi_test(int iIndex, int iNumParams, char *pcParam[],
                            char *pcValue[]) {
  DPRINTF("TEST CGI handler called with index %d\n", iIndex);
  return "/test.shtml";
}

/**
 * @brief Show the folder content
 *
 * @param iIndex The index of the CGI handler.
 * @param iNumParams The number of parameters passed to the CGI handler.
 * @param pcParam An array of parameter names.
 * @param pcValue An array of parameter values.
 * @return The URL of the page to redirect to after the floppy disk image is
 * selected.
 */
static const char *cgi_folder(int iIndex, int iNumParams, char *pcParam[],
                              char *pcValue[]) {
  DPRINTF("FOLDER CGI handler called with index %d\n", iIndex);
  /* Parse 'folder' query parameter */
  const char *req_folder = NULL;
  char decoded_folder[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  char folder_abs[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  for (int i = 0; i < iNumParams; i++) {
    if (strcmp(pcParam[i], "folder") == 0) {
      req_folder = pcValue[i];
      if (!url_decode(req_folder, decoded_folder, sizeof(decoded_folder)) ||
          !normalize_and_validate_path(decoded_folder, folder_abs,
                                       sizeof(folder_abs))) {
        DPRINTF("Invalid folder parameter: %s\n", req_folder);
        /* Return empty JSON array */
        strcpy(json_buff, "[]");
        return "/json.shtml";
      }
      DPRINTF("Folder parameter: %s\n", folder_abs);
      break;
    }
  }
  if (req_folder == NULL) {
    DPRINTF("No folder parameter provided\n");
    /* Return empty JSON array */
    strcpy(json_buff, "[]");
    return "/json.shtml";
  }
  DPRINTF("Listing subfolders of: %s\n", folder_abs);
  /* Prepare JSON array */
  size_t json_len = 0;
  json_buff[0] = '\0';
  if (!json_appendf(json_buff, sizeof(json_buff), &json_len, "[")) {
    strcpy(json_buff, "[]");
    return "/json.shtml";
  }
  /* FatFS list directories in the given folder */
  DIR dir;
  FILINFO fno;
  FRESULT fr;
  bool apps_installed_found = false;
  bool truncated = false;
  fr = f_opendir(&dir, folder_abs);
  if (fr != FR_OK) {
    DPRINTF("Failed to open directory %s, error %d\n", folder_abs, fr);
    /* Return empty JSON array */
    strcpy(json_buff, "[]");
    return "/json.shtml";
  }
  for (;;) {
    fr = f_readdir(&dir, &fno);
    if (fr != FR_OK || fno.fname[0] == '\0') break;
    if (fno.fattrib & AM_DIR) {
      DPRINTF("DIR: %s/%s\n", folder_abs, fno.fname);
      /* Append folder name to JSON array */
      if (!json_appendf(json_buff, sizeof(json_buff), &json_len, "\"%s\",",
                        fno.fname)) {
        truncated = true;
        break;
      }
      apps_installed_found = true;
    }
  }
  f_closedir(&dir);
  /* Close JSON array; remove trailing comma if present */
  if (apps_installed_found && json_len > 1 && json_buff[json_len - 1] == ',') {
    json_buff[json_len - 1] = '\0';
    json_len--;
  }
  if (!json_appendf(json_buff, sizeof(json_buff), &json_len, "]")) {
    strcpy(json_buff, "[]");
    return "/json.shtml";
  }
  if (truncated) {
    DPRINTF("Folder listing truncated to fit JSON buffer\n");
  }
  if (!apps_installed_found) {
    DPRINTF("No subfolders found in %s\n", folder_abs);
  }
  /* Return JSON page (json_buff contains array or empty) */
  return "/json.shtml";
}
// CGI: make directory
static const char *cgi_mkdir(int iIndex, int iNumParams, char *pcParam[],
                             char *pcValue[]) {
  const char *folder = NULL, *src = NULL;
  char df[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  char ds[MNGR_HTTPD_MAX_NAME_LEN] = {0};
  char folder_abs[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  char *path = NULL;
  for (int i = 0; i < iNumParams; i++) {
    if (!strcmp(pcParam[i], "folder")) folder = pcValue[i];
    if (!strcmp(pcParam[i], "src")) src = pcValue[i];
  }
  if (!folder || !src) {
    strcpy(json_buff, "{\"error\":\"missing parameters\"}");
    return "/json.shtml";
  }
  path = calloc(1, MNGR_HTTPD_MAX_PATH_LEN);
  if (!path) {
    strcpy(json_buff, "{\"error\":\"out of memory\"}");
    return "/json.shtml";
  }
  if (!url_decode(folder, df, sizeof(df)) || !url_decode(src, ds, sizeof(ds)) ||
      !normalize_and_validate_path(df, folder_abs, sizeof(folder_abs)) ||
      !join_root_folder_name(folder_abs, ds, path, MNGR_HTTPD_MAX_PATH_LEN)) {
    strcpy(json_buff, "{\"error\":\"invalid encoding\"}");
    free(path);
    return "/json.shtml";
  }
  FRESULT r = f_mkdir(path);
  free(path);
  if (r != FR_OK)
    snprintf(json_buff, sizeof(json_buff), "{\"error\":\"mkdir failed %d\"}",
             r);
  else
    strcpy(json_buff, "{\"status\":\"created\"}");
  return "/json.shtml";
}

/**
 * @brief Download the selected app from the folder and URL parameters.
 */
const char *cgi_download(int iIndex, int iNumParams, char *pcParam[],
                         char *pcValue[]) {
  DPRINTF("cgi_download called with index %d\n", iIndex);

  char decoded_folder[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  char folder_abs[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  char decoded_url[DOWNLOAD_BUFFLINE_SIZE] = {0};
  bool has_folder = false, has_url = false;
  for (int i = 0; i < iNumParams; i++) {
    if (strcmp(pcParam[i], "folder") == 0) {
      if (!url_decode(pcValue[i], decoded_folder, sizeof(decoded_folder)) ||
          !normalize_and_validate_path(decoded_folder, folder_abs,
                                       sizeof(folder_abs))) {
        DPRINTF("Invalid folder parameter: %s\n", pcValue[i]);
        static char error_url[MNGR_HTTPD_ERROR_URL_LEN];
        snprintf(error_url, sizeof(error_url),
                 "/error.shtml?error=%d&error_msg=Invalid%%20folder",
                 MNGR_HTTPD_RESPONSE_BAD_REQUEST);
        return error_url;
      }
      has_folder = true;
    } else if (strcmp(pcParam[i], "url") == 0) {
      if (!url_decode(pcValue[i], decoded_url, sizeof(decoded_url))) {
        DPRINTF("Invalid URL parameter: %s\n", pcValue[i]);
        static char error_url[MNGR_HTTPD_ERROR_URL_LEN];
        snprintf(error_url, sizeof(error_url),
                 "/error.shtml?error=%d&error_msg=Invalid%%20url",
                 MNGR_HTTPD_RESPONSE_BAD_REQUEST);
        return error_url;
      }
      has_url = true;
    }
  }
  if (!has_folder || !has_url) {
    DPRINTF("Missing folder or URL parameter\n");
    static char error_url[MNGR_HTTPD_ERROR_URL_LEN];
    snprintf(error_url, sizeof(error_url),
             "/error.shtml?error=%d&error_msg=Bad%%20request",
             MNGR_HTTPD_RESPONSE_BAD_REQUEST);
    return error_url;
  }
  DPRINTF("Download request: folder=%s, url=%s\n", folder_abs, decoded_url);
  download_setDstFolder(folder_abs);
  download_setUrl(decoded_url);
  download_setStatus(DOWNLOAD_STATUS_REQUESTED);

  return "/downloading.shtml";
}

/**
 * @brief Execute a ls-like operation to list all content
 *
 * @param iIndex The index of the CGI handler.
 * @param iNumParams The number of parameters passed to the CGI handler.
 * @param pcParam An array of parameter names.
 * @param pcValue An array of parameter values.
 * @return The URL of the page to redirect to after the floppy disk image is
 * selected.
 */
static const char *cgi_ls(int iIndex, int iNumParams, char *pcParam[],
                          char *pcValue[]) {
  DPRINTF("LS CGI handler called with index %d\n", iIndex);
  /* Parse 'folder' query parameter */
  const char *req_folder = NULL;
  char decoded_folder[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  char folder_abs[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  for (int i = 0; i < iNumParams; i++) {
    if (strcmp(pcParam[i], "folder") == 0) {
      req_folder = pcValue[i];
      if (!url_decode(req_folder, decoded_folder, sizeof(decoded_folder)) ||
          !normalize_and_validate_path(decoded_folder, folder_abs,
                                       sizeof(folder_abs))) {
        DPRINTF("Invalid folder parameter: %s\n", req_folder);
        /* Return empty JSON array */
        strcpy(json_buff, "[]");
        return "/json.shtml";
      }
      DPRINTF("Folder parameter: %s\n", folder_abs);
      break;
    }
  }
  if (req_folder == NULL) {
    DPRINTF("No folder parameter provided\n");
    /* Return empty JSON array */
    strcpy(json_buff, "[]");
    return "/json.shtml";
  }
  DPRINTF("Listing entries of: %s\n", folder_abs);
  // Parse optional pagination start index
  int nextItem = 0;
  for (int j = 0; j < iNumParams; j++) {
    if (strcmp(pcParam[j], "nextItem") == 0) {
      nextItem = atoi(pcValue[j]);
      DPRINTF("cgi_ls: start from item %d\n", nextItem);
      break;
    }
  }
  unsigned idx = 0;
  bool truncated = false;
  /* Prepare JSON array */
  size_t json_len = 0;
  json_buff[0] = '\0';
  if (!json_appendf(json_buff, sizeof(json_buff), &json_len, "[")) {
    strcpy(json_buff, "[]");
    return "/json.shtml";
  }
  /* FatFS list directory entries */
  DIR dir;
  FILINFO fno;
  FRESULT fr;
  bool apps_installed_found = false;
  fr = f_opendir(&dir, folder_abs);
  if (fr != FR_OK) {
    DPRINTF("Failed to open directory %s, error %d\n", folder_abs, fr);
    /* Return empty JSON array */
    strcpy(json_buff, "[]");
    return "/json.shtml";
  }
  for (;;) {
    fr = f_readdir(&dir, &fno);
    if (fr != FR_OK || fno.fname[0] == '\0') break;
    // Skip until reaching nextItem index
    if (idx++ < nextItem) continue;
    /* Build JSON object for each entry */
    /* Combine date and time into a single ts field */
    unsigned ts = ((unsigned)fno.fdate << 16) | (unsigned)fno.ftime;
    if (!json_appendf(json_buff, sizeof(json_buff), &json_len,
                      "{\"n\":\"%s\",\"a\":%u,\"s\":%lu,\"t\":%u},", fno.fname,
                      (unsigned)fno.fattrib, (unsigned long)fno.fsize, ts)) {
      DPRINTF("JSON payload too large (%u chars), truncating listing\n",
              (unsigned)json_len);
      truncated = true;
      break;
    }
    apps_installed_found = true;
  }
  f_closedir(&dir);
  /* Close JSON array, handle truncation sentinel */
  if (truncated) {
    if (json_len > 1 && json_buff[json_len - 1] == ',') {
      json_buff[json_len - 1] = '\0';
      json_len--;
    }
    // Add a sentinel empty object to signal "more data available".
    if (json_len > 1) {
      (void)json_appendf(json_buff, sizeof(json_buff), &json_len, ",{}]");
    } else {
      (void)json_appendf(json_buff, sizeof(json_buff), &json_len, "{}]");
    }
  } else if (apps_installed_found) {
    /* Remove trailing comma if any */
    if (json_len > 1 && json_buff[json_len - 1] == ',') {
      json_buff[json_len - 1] = '\0';
      json_len--;
    }
    (void)json_appendf(json_buff, sizeof(json_buff), &json_len, "]");
  } else {
    /* No entries and not truncated */
    strcpy(json_buff, "[]");
  }
  if (!apps_installed_found) {
    DPRINTF("No subfolders found in %s\n", folder_abs);
  }
  /* Return JSON page (json_buff contains array or empty) */
  return "/json.shtml";
}

// Upload context for resumable uploads
#define MAX_UPLOAD_CONTEXTS 4
typedef struct {
  char token[MNGR_HTTPD_MAX_TOKEN_LEN];
  FIL file;
  bool in_use;
  bool file_open;
} upload_ctx_t;
static upload_ctx_t upload_contexts[MAX_UPLOAD_CONTEXTS] = {0};
// Per-connection state for POST-based chunk upload
typedef struct {
  void *connection;
  upload_ctx_t *ctx;
  bool in_use;
  bool had_error;
} post_chunk_state_t;
static post_chunk_state_t post_chunk_states[MAX_UPLOAD_CONTEXTS] = {0};

// Find context by token
static upload_ctx_t *find_upload_ctx(const char *token) {
  for (int i = 0; i < MAX_UPLOAD_CONTEXTS; i++) {
    if (upload_contexts[i].in_use &&
        strcmp(upload_contexts[i].token, token) == 0) {
      return &upload_contexts[i];
    }
  }
  return NULL;
}

// Allocate new context
static upload_ctx_t *alloc_upload_ctx(const char *token) {
  for (int i = 0; i < MAX_UPLOAD_CONTEXTS; i++) {
    if (!upload_contexts[i].in_use) {
      upload_contexts[i].in_use = true;
      upload_contexts[i].file_open = false;
      strncpy(upload_contexts[i].token, token,
              sizeof(upload_contexts[i].token) - 1);
      upload_contexts[i].token[sizeof(upload_contexts[i].token) - 1] = '\0';
      return &upload_contexts[i];
    }
  }
  return NULL;
}

static post_chunk_state_t *find_post_chunk_state(void *connection) {
  for (int i = 0; i < MAX_UPLOAD_CONTEXTS; i++) {
    if (post_chunk_states[i].in_use &&
        post_chunk_states[i].connection == connection) {
      return &post_chunk_states[i];
    }
  }
  return NULL;
}

static void free_post_chunk_state(post_chunk_state_t *st) {
  if (!st) return;
  st->connection = NULL;
  st->ctx = NULL;
  st->in_use = false;
  st->had_error = false;
}

static post_chunk_state_t *alloc_post_chunk_state(void *connection,
                                                  upload_ctx_t *ctx) {
  post_chunk_state_t *existing = find_post_chunk_state(connection);
  if (existing) {
    existing->ctx = ctx;
    existing->had_error = false;
    return existing;
  }
  for (int i = 0; i < MAX_UPLOAD_CONTEXTS; i++) {
    if (!post_chunk_states[i].in_use) {
      post_chunk_states[i].in_use = true;
      post_chunk_states[i].connection = connection;
      post_chunk_states[i].ctx = ctx;
      post_chunk_states[i].had_error = false;
      return &post_chunk_states[i];
    }
  }
  return NULL;
}

// Download context for chunked downloads
#define MAX_DOWNLOAD_CONTEXTS 4
typedef struct {
  char token[MNGR_HTTPD_MAX_TOKEN_LEN];
  FIL file;
  bool in_use;
  bool file_open;
} download_ctx_t;
static download_ctx_t download_contexts[MAX_DOWNLOAD_CONTEXTS] = {0};

// Find download context by token
static download_ctx_t *find_download_ctx(const char *token) {
  for (int i = 0; i < MAX_DOWNLOAD_CONTEXTS; i++) {
    if (download_contexts[i].in_use &&
        strcmp(download_contexts[i].token, token) == 0) {
      return &download_contexts[i];
    }
  }
  return NULL;
}

// Allocate new download context
static download_ctx_t *alloc_download_ctx(const char *token) {
  for (int i = 0; i < MAX_DOWNLOAD_CONTEXTS; i++) {
    if (!download_contexts[i].in_use) {
      download_contexts[i].in_use = true;
      download_contexts[i].file_open = false;
      strncpy(download_contexts[i].token, token,
              sizeof(download_contexts[i].token) - 1);
      download_contexts[i].token[sizeof(download_contexts[i].token) - 1] = '\0';
      return &download_contexts[i];
    }
  }
  return NULL;
}

// Free download context and close file
static void free_download_ctx(download_ctx_t *ctx) {
  if (!ctx) return;
  if (ctx->file_open) {
    f_close(&ctx->file);
  }
  ctx->file_open = false;
  ctx->in_use = false;
  ctx->token[0] = '\0';
}

// Free context and close file
static void free_upload_ctx(upload_ctx_t *ctx) {
  if (!ctx) return;
  if (ctx->file_open) {
    f_close(&ctx->file);
  }
  ctx->file_open = false;
  ctx->in_use = false;
  ctx->token[0] = '\0';
}

// CGI: start upload
static const char *cgi_upload_start(int iIndex, int iNumParams, char *pcParam[],
                                    char *pcValue[]) {
  const char *token = NULL, *folder = NULL, *src = NULL;
  char decoded_folder[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  char decoded_src[MNGR_HTTPD_MAX_NAME_LEN] = {0};
  char folder_abs[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  char decoded_path[MNGR_HTTPD_MAX_PATH_LEN] = {0};
  for (int i = 0; i < iNumParams; i++) {
    if (strcmp(pcParam[i], "token") == 0) token = pcValue[i];
    if (strcmp(pcParam[i], "folder") == 0) folder = pcValue[i];
    if (strcmp(pcParam[i], "src") == 0) src = pcValue[i];
  }
  if (!token || !folder || !src) {
    strcpy(json_buff, "{\"error\":\"missing parameters\"}");
    return "/json.shtml";
  }
  if (!url_decode(folder, decoded_folder, sizeof(decoded_folder)) ||
      !url_decode(src, decoded_src, sizeof(decoded_src)) ||
      !normalize_and_validate_path(decoded_folder, folder_abs,
                                   sizeof(folder_abs)) ||
      !join_root_folder_name(folder_abs, decoded_src, decoded_path,
                             sizeof(decoded_path))) {
    strcpy(json_buff, "{\"error\":\"invalid path parameters\"}");
    return "/json.shtml";
  }
  upload_ctx_t *ctx = alloc_upload_ctx(token);
  if (!ctx) {
    strcpy(json_buff, "{\"error\":\"no context available\"}");
    return "/json.shtml";
  }
  // Open file for writing
  FRESULT res = f_open(&ctx->file, decoded_path, FA_WRITE | FA_CREATE_ALWAYS);
  if (res != FR_OK) {
    free_upload_ctx(ctx);
    strcpy(json_buff, "{\"error\":\"cannot open file\"}");
    return "/json.shtml";
  }
  ctx->file_open = true;
  // Return status, chunk size, and preferred method for client
  snprintf(json_buff, sizeof(json_buff),
           "{\"status\":\"started\",\"chunkSize\":%d,\"method\":\"%s\"}",
           UPLOAD_CHUNK_SIZE, UPLOAD_CHUNK_METHOD);

  return "/json.shtml";
}

// CGI: upload chunk (base64 payload)
static const char *cgi_upload_chunk(int iIndex, int iNumParams, char *pcParam[],
                                    char *pcValue[]) {
  const char *token = NULL, *chunkStr = NULL, *payload = NULL;
  for (int i = 0; i < iNumParams; i++) {
    if (strcmp(pcParam[i], "token") == 0) token = pcValue[i];
    if (strcmp(pcParam[i], "chunk") == 0) chunkStr = pcValue[i];
    if (strcmp(pcParam[i], "payload") == 0) payload = pcValue[i];
  }
  upload_ctx_t *ctx = token ? find_upload_ctx(token) : NULL;
  if (!ctx || !chunkStr || !payload) {
    strcpy(json_buff, "{\"error\":\"invalid parameters\"}");
    return "/json.shtml";
  }
  int chunk = atoi(chunkStr);
  // URL-decode the base64 payload parameter
  size_t plen = strlen(payload) + 1;
  char *decodedPayload = malloc(plen);
  if (!decodedPayload || !url_decode(payload, decodedPayload, plen)) {
    free(decodedPayload);
    strcpy(json_buff, "{\"error\":\"invalid payload encoding\"}");
    return "/json.shtml";
  }
  // Base64-decode the payload
  const size_t inLen = strlen(decodedPayload);
  size_t decodedLen = 0;
  unsigned char *buffer = NULL;

  if (inLen > 0) {
    // Conservative output capacity: (inLen/4 + 1) * 3 covers padding and avoids
    // under-allocation.
    const size_t outCap = ((inLen / 4) + 1) * 3;
    buffer = malloc(outCap);
    if (!buffer) {
      free(decodedPayload);
      strcpy(json_buff, "{\"error\":\"out of memory\"}");
      return "/json.shtml";
    }
    if (mbedtls_base64_decode(buffer, outCap, &decodedLen,
                              (const unsigned char *)decodedPayload,
                              inLen) != 0) {
      free(buffer);
      free(decodedPayload);
      strcpy(json_buff, "{\"error\":\"invalid base64\"}");
      return "/json.shtml";
    }
  }
  free(decodedPayload);
  // Seek to chunk offset using fixed chunk size
  f_lseek(&ctx->file, (DWORD)(chunk * UPLOAD_CHUNK_SIZE));
  if (decodedLen > 0) {
    UINT written;
    FRESULT res = f_write(&ctx->file, buffer, decodedLen, &written);
    free(buffer);
    if (res != FR_OK || written != decodedLen) {
      strcpy(json_buff, "{\"error\":\"write failed\"}");
      return "/json.shtml";
    }
  } else {
    free(buffer);
  }
  strcpy(json_buff, "{\"status\":\"chunk_ok\"}");
  return "/json.shtml";
}

// CGI: end upload
static const char *cgi_upload_end(int iIndex, int iNumParams, char *pcParam[],
                                  char *pcValue[]) {
  const char *token = NULL;
  for (int i = 0; i < iNumParams; i++)
    if (strcmp(pcParam[i], "token") == 0) token = pcValue[i];
  upload_ctx_t *ctx = token ? find_upload_ctx(token) : NULL;
  if (!ctx) {
    strcpy(json_buff, "{\"error\":\"invalid token\"}");
    return "/json.shtml";
  }
  free_upload_ctx(ctx);
  strcpy(json_buff, "{\"status\":\"completed\"}");
  return "/json.shtml";
}

// CGI: cancel upload
static const char *cgi_upload_cancel(int iIndex, int iNumParams,
                                     char *pcParam[], char *pcValue[]) {
  const char *token = NULL;
  for (int i = 0; i < iNumParams; i++)
    if (strcmp(pcParam[i], "token") == 0) token = pcValue[i];
  upload_ctx_t *ctx = token ? find_upload_ctx(token) : NULL;
  if (!ctx) {
    strcpy(json_buff, "{\"error\":\"invalid token\"}");
    return "/json.shtml";
  }
  // Optionally delete partial file
  char path[MNGR_HTTPD_MAX_PATH_LEN];
  f_getcwd(path, sizeof(path));  // stub, adjust if needed
  // Remove file using fullpath stored? Skipped
  free_upload_ctx(ctx);
  strcpy(json_buff, "{\"status\":\"cancelled\"}");
  return "/json.shtml";
}

// CGI: start download
static const char *cgi_download_start(int iIndex, int iNumParams,
                                      char *pcParam[], char *pcValue[]) {
  const char *token = NULL, *folder = NULL, *src = NULL;
  char decoded_folder[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  char decoded_src[MNGR_HTTPD_MAX_NAME_LEN] = {0};
  char folder_abs[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  char decoded_path[MNGR_HTTPD_MAX_PATH_LEN] = {0};
  for (int i = 0; i < iNumParams; i++) {
    if (strcmp(pcParam[i], "token") == 0) token = pcValue[i];
    if (strcmp(pcParam[i], "folder") == 0) folder = pcValue[i];
    if (strcmp(pcParam[i], "src") == 0) src = pcValue[i];
  }
  if (!token || !folder || !src) {
    strcpy(json_buff, "{\"error\":\"missing parameters\"}");
    return "/json.shtml";
  }
  if (!url_decode(folder, decoded_folder, sizeof(decoded_folder)) ||
      !url_decode(src, decoded_src, sizeof(decoded_src)) ||
      !normalize_and_validate_path(decoded_folder, folder_abs,
                                   sizeof(folder_abs)) ||
      !join_root_folder_name(folder_abs, decoded_src, decoded_path,
                             sizeof(decoded_path))) {
    strcpy(json_buff, "{\"error\":\"invalid path parameters\"}");
    return "/json.shtml";
  }
  download_ctx_t *ctx = alloc_download_ctx(token);
  if (!ctx) {
    strcpy(json_buff, "{\"error\":\"no context available\"}");
    return "/json.shtml";
  }
  FRESULT res = f_open(&ctx->file, decoded_path, FA_READ);
  if (res != FR_OK) {
    free_download_ctx(ctx);
    strcpy(json_buff, "{\"error\":\"cannot open file\"}");
    return "/json.shtml";
  }
  ctx->file_open = true;
  DWORD size = f_size(&ctx->file);
  snprintf(json_buff, sizeof(json_buff),
           "{\"status\":\"started\",\"chunkSize\":%d,\"fileSize\":%lu}",
           DOWNLOAD_CHUNK_SIZE, (unsigned long)size);
  return "/json.shtml";
}

// CGI: download chunk
static const char *cgi_download_chunk(int iIndex, int iNumParams,
                                      char *pcParam[], char *pcValue[]) {
  const char *token = NULL, *chunkStr = NULL;
  unsigned char *rawbuf = NULL;
  unsigned char *b64buf = NULL;
  for (int i = 0; i < iNumParams; i++) {
    if (strcmp(pcParam[i], "token") == 0) token = pcValue[i];
    if (strcmp(pcParam[i], "chunk") == 0) chunkStr = pcValue[i];
  }
  download_ctx_t *ctx = token ? find_download_ctx(token) : NULL;
  if (!ctx || !chunkStr) {
    strcpy(json_buff, "{\"error\":\"invalid parameters\"}");
    return "/json.shtml";
  }
  int chunk = atoi(chunkStr);
  DWORD offset = (DWORD)chunk * DOWNLOAD_CHUNK_SIZE;
  f_lseek(&ctx->file, offset);
  rawbuf = malloc(DOWNLOAD_CHUNK_SIZE);
  b64buf = malloc(MAX_JSON_PAYLOAD_SIZE);
  if (!rawbuf || !b64buf) {
    strcpy(json_buff, "{\"error\":\"out of memory\"}");
    goto cleanup;
  }
  UINT readBytes = 0;
  FRESULT res = f_read(&ctx->file, rawbuf, DOWNLOAD_CHUNK_SIZE, &readBytes);
  if (res != FR_OK) {
    strcpy(json_buff, "{\"error\":\"read failed\"}");
    goto cleanup;
  }
  size_t olen = 0;
  if (mbedtls_base64_encode(b64buf, MAX_JSON_PAYLOAD_SIZE, &olen, rawbuf,
                            readBytes) != 0) {
    strcpy(json_buff, "{\"error\":\"base64 encode failed\"}");
    goto cleanup;
  }
  // JSON response with base64 data
  snprintf(json_buff, sizeof(json_buff),
           "{\"status\":\"chunk\",\"length\":%u,\"data\":\"%.*s\"}",
           (unsigned)readBytes, (int)olen, b64buf);
cleanup:
  free(b64buf);
  free(rawbuf);
  return "/json.shtml";
}

// CGI: end download
static const char *cgi_download_end(int iIndex, int iNumParams, char *pcParam[],
                                    char *pcValue[]) {
  const char *token = NULL;
  for (int i = 0; i < iNumParams; i++)
    if (strcmp(pcParam[i], "token") == 0) token = pcValue[i];
  download_ctx_t *ctx = token ? find_download_ctx(token) : NULL;
  if (!ctx) {
    strcpy(json_buff, "{\"error\":\"invalid token\"}");
    return "/json.shtml";
  }
  free_download_ctx(ctx);
  strcpy(json_buff, "{\"status\":\"completed\"}");
  return "/json.shtml";
}

// CGI: cancel download
static const char *cgi_download_cancel(int iIndex, int iNumParams,
                                       char *pcParam[], char *pcValue[]) {
  const char *token = NULL;
  for (int i = 0; i < iNumParams; i++)
    if (strcmp(pcParam[i], "token") == 0) token = pcValue[i];
  download_ctx_t *ctx = token ? find_download_ctx(token) : NULL;
  if (!ctx) {
    strcpy(json_buff, "{\"error\":\"invalid token\"}");
    return "/json.shtml";
  }
  free_download_ctx(ctx);
  strcpy(json_buff, "{\"status\":\"cancelled\"}");
  return "/json.shtml";
}

// CGI: rename file
static const char *cgi_ren(int iIndex, int iNumParams, char *pcParam[],
                           char *pcValue[]) {
  DPRINTF("REN CGI handler called with index %d, numParams %d\n", iIndex,
          iNumParams);
  const char *folder = NULL, *src = NULL, *dst = NULL;
  char *df = NULL, *ds = NULL, *dd = NULL, *folder_abs = NULL;
  char *from = NULL, *to = NULL;
  for (int i = 0; i < iNumParams; i++) {
    if (!strcmp(pcParam[i], "folder")) folder = pcValue[i];
    if (!strcmp(pcParam[i], "src")) src = pcValue[i];
    if (!strcmp(pcParam[i], "dst")) dst = pcValue[i];
  }
  DPRINTF("cgi_ren params: folder='%s' src='%s' dst='%s'\n",
          folder ? folder : "(null)", src ? src : "(null)",
          dst ? dst : "(null)");

  const char *ret = "/json.shtml";
  if (!folder || !src || !dst) {
    strcpy(json_buff, "{\"error\":\"missing parameters\"}");
    return ret;
  }

  df = calloc(1, MNGR_HTTPD_MAX_FOLDER_LEN);
  ds = calloc(1, MNGR_HTTPD_MAX_NAME_LEN);
  dd = calloc(1, MNGR_HTTPD_MAX_NAME_LEN);
  folder_abs = calloc(1, MNGR_HTTPD_MAX_FOLDER_LEN);
  from = calloc(1, MNGR_HTTPD_MAX_PATH_LEN);
  to = calloc(1, MNGR_HTTPD_MAX_PATH_LEN);
  if (!df || !ds || !dd || !folder_abs || !from || !to) {
    strcpy(json_buff, "{\"error\":\"out of memory\"}");
    goto cleanup;
  }

  if (!url_decode(folder, df, MNGR_HTTPD_MAX_FOLDER_LEN) ||
      !url_decode(src, ds, MNGR_HTTPD_MAX_NAME_LEN) ||
      !url_decode(dst, dd, MNGR_HTTPD_MAX_NAME_LEN) ||
      !normalize_and_validate_path(df, folder_abs, MNGR_HTTPD_MAX_FOLDER_LEN)) {
    DPRINTF("cgi_ren decode/validate failed: df='%s' ds='%s' dd='%s'\n", df, ds,
            dd);
    strcpy(json_buff, "{\"error\":\"invalid encoding\"}");
    goto cleanup;
  }
  DPRINTF("cgi_ren decoded: folder_abs='%s' src='%s' dst='%s'\n", folder_abs,
          ds, dd);
  if (!join_root_folder_name(folder_abs, ds, from, MNGR_HTTPD_MAX_PATH_LEN) ||
      !join_root_folder_name(folder_abs, dd, to, MNGR_HTTPD_MAX_PATH_LEN)) {
    DPRINTF("cgi_ren join path failed: from='%s' to='%s'\n", from, to);
    strcpy(json_buff, "{\"error\":\"invalid path\"}");
    goto cleanup;
  }
  DPRINTF("cgi_ren renaming: '%s' -> '%s'\n", from, to);
  FRESULT r = f_rename(from, to);
  DPRINTF("cgi_ren f_rename result: %d\n", (int)r);
  if (r != FR_OK)
    snprintf(json_buff, sizeof(json_buff), "{\"error\":\"rename failed %d\"}",
             r);
  else
    strcpy(json_buff, "{\"status\":\"renamed\"}");

cleanup:
  free(to);
  free(from);
  free(folder_abs);
  free(dd);
  free(ds);
  free(df);
  return ret;
}

// CGI: delete file
// Recursively delete path (file or directory)
// Delete file or directory only if directory is empty
static FRESULT delete_path(const char *path) {
  FILINFO fno;
  FRESULT fr = f_stat(path, &fno);
  if (fr != FR_OK) return fr;
  if (fno.fattrib & AM_DIR) {
    // Check if directory is empty
    DIR dir;
    fr = f_opendir(&dir, path);
    if (fr != FR_OK) return fr;
    fr = f_readdir(&dir, &fno);
    f_closedir(&dir);
    if (fr != FR_OK) return fr;
    // If first entry name is not empty, directory has contents
    if (fno.fname[0] != '\0') return FR_DENIED;
    // Empty directory -> remove
    return f_unlink(path);
  }
  // Regular file
  return f_unlink(path);
}
static const char *cgi_del(int iIndex, int iNumParams, char *pcParam[],
                           char *pcValue[]) {
  const char *folder = NULL, *src = NULL;
  char df[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  char ds[MNGR_HTTPD_MAX_NAME_LEN] = {0};
  char folder_abs[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  char *path = NULL;
  for (int i = 0; i < iNumParams; i++) {
    if (!strcmp(pcParam[i], "folder")) folder = pcValue[i];
    if (!strcmp(pcParam[i], "src")) src = pcValue[i];
  }
  if (!folder || !src) {
    strcpy(json_buff, "{\"error\":\"missing parameters\"}");
    return "/json.shtml";
  }
  if (!url_decode(folder, df, sizeof(df)) || !url_decode(src, ds, sizeof(ds)) ||
      !normalize_and_validate_path(df, folder_abs, sizeof(folder_abs))) {
    strcpy(json_buff, "{\"error\":\"invalid encoding\"}");
    return "/json.shtml";
  }
  path = calloc(1, MNGR_HTTPD_MAX_PATH_LEN);
  if (!path) {
    strcpy(json_buff, "{\"error\":\"out of memory\"}");
    return "/json.shtml";
  }
  if (!join_root_folder_name(folder_abs, ds, path, MNGR_HTTPD_MAX_PATH_LEN)) {
    strcpy(json_buff, "{\"error\":\"invalid path\"}");
    free(path);
    return "/json.shtml";
  }
  FRESULT r = delete_path(path);
  free(path);
  if (r == FR_DENIED) {
    snprintf(json_buff, sizeof(json_buff),
             "{\"error\":\"directory not empty\"}");
  } else if (r != FR_OK) {
    snprintf(json_buff, sizeof(json_buff), "{\"error\":\"delete failed %d\"}",
             r);
  } else {
    strcpy(json_buff, "{\"status\":\"deleted\"}");
  }
  return "/json.shtml";
}
// CGI: set file attributes (hidden & read-only)
static const char *cgi_attr(int iIndex, int iNumParams, char *pcParam[],
                            char *pcValue[]) {
  const char *folder = NULL, *src = NULL, *hidden_s = NULL, *readonly_s = NULL;
  char df[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  char ds[MNGR_HTTPD_MAX_NAME_LEN] = {0};
  char *path = NULL;
  char folder_abs[MNGR_HTTPD_MAX_FOLDER_LEN] = {0};
  for (int i = 0; i < iNumParams; i++) {
    if (!strcmp(pcParam[i], "folder")) folder = pcValue[i];
    if (!strcmp(pcParam[i], "src")) src = pcValue[i];
    if (!strcmp(pcParam[i], "hidden")) hidden_s = pcValue[i];
    if (!strcmp(pcParam[i], "readonly")) readonly_s = pcValue[i];
  }
  if (!folder || !src || !hidden_s || !readonly_s) {
    strcpy(json_buff, "{\"error\":\"missing parameters\"}");
    return "/json.shtml";
  }
  path = calloc(1, MNGR_HTTPD_MAX_PATH_LEN);
  if (!path) {
    strcpy(json_buff, "{\"error\":\"out of memory\"}");
    return "/json.shtml";
  }
  if (!url_decode(folder, df, sizeof(df)) || !url_decode(src, ds, sizeof(ds)) ||
      !normalize_and_validate_path(df, folder_abs, sizeof(folder_abs)) ||
      !join_root_folder_name(folder_abs, ds, path, MNGR_HTTPD_MAX_PATH_LEN)) {
    strcpy(json_buff, "{\"error\":\"invalid encoding\"}");
    free(path);
    return "/json.shtml";
  }
  int hide = atoi(hidden_s);
  int ro = atoi(readonly_s);
  // Build attribute mask
  BYTE attr = 0;
  if (ro) attr |= AM_RDO;
  if (hide) attr |= AM_HID;
  // Apply only hidden and read-only bits
  FRESULT r = f_chmod(path, attr, AM_RDO | AM_HID);
  free(path);
  if (r != FR_OK)
    snprintf(json_buff, sizeof(json_buff), "{\"error\":\"chmod failed %d\"}",
             r);
  else
    strcpy(json_buff, "{\"status\":\"attributes updated\"}");
  return "/json.shtml";
}

/**
 * @brief Array of CGI handlers for floppy select and eject operations.
 *
 * This array contains the mappings between the CGI paths and the corresponding
 * handler functions for selecting and ejecting floppy disk images for drive A
 * and drive B.
 */
static const tCGI cgi_handlers[] = {
    {"/test.cgi", cgi_test},
    {"/folder.cgi", cgi_folder},
    {"/download.cgi", cgi_download},
    {"/ls.cgi", cgi_ls},
    {"/upload_start.cgi", cgi_upload_start},
    {"/upload_chunk.cgi", cgi_upload_chunk},
    {"/upload_end.cgi", cgi_upload_end},
    {"/upload_cancel.cgi", cgi_upload_cancel},
    {"/ren.cgi", cgi_ren},
    {"/mkdir.cgi", cgi_mkdir},
    {"/del.cgi", cgi_del},
    {"/attr.cgi", cgi_attr},
    {"/download_start.cgi", cgi_download_start},
    {"/download_chunk.cgi", cgi_download_chunk},
    {"/download_end.cgi", cgi_download_end},
    {"/download_cancel.cgi", cgi_download_cancel}};

/**
 * @brief Initializes the HTTP server with optional SSI tags, CGI handlers, and
 * an SSI handler function.
 *
 * This function initializes the HTTP server and sets up the provided Server
 * Side Include (SSI) tags, Common Gateway Interface (CGI) handlers, and SSI
 * handler function. It first calls the httpd_init() function to initialize the
 * HTTP server.
 *
 * The filesystem for the HTTP server is in the 'fs' directory in the project
 * root.
 *
 * @param ssi_tags An array of strings representing the SSI tags to be used in
 * the server-side includes.
 * @param num_tags The number of SSI tags in the ssi_tags array.
 * @param ssi_handler_func A pointer to the function that handles SSI tags.
 * @param cgi_handlers An array of tCGI structures representing the CGI handlers
 * to be used.
 * @param num_cgi_handlers The number of CGI handlers in the cgi_handlers array.
 */
static void httpd_server_init(const char *ssi_tags[], size_t num_tags,
                              tSSIHandler ssi_handler_func,
                              const tCGI *cgi_handlers,
                              size_t num_cgi_handlers) {
  httpd_init();

  // SSI Initialization
  if (num_tags > 0) {
    for (size_t i = 0; i < num_tags; i++) {
      LWIP_ASSERT("tag too long for LWIP_HTTPD_MAX_TAG_NAME_LEN",
                  strlen(ssi_tags[i]) <= LWIP_HTTPD_MAX_TAG_NAME_LEN);
    }
    http_set_ssi_handler(ssi_handler_func, ssi_tags, num_tags);
  } else {
    DPRINTF("No SSI tags defined.\n");
  }

  // CGI Initialization
  if (num_cgi_handlers > 0) {
    http_set_cgi_handlers(cgi_handlers, num_cgi_handlers);
  } else {
    DPRINTF("No CGI handlers defined.\n");
  }

  DPRINTF("HTTP server initialized.\n");
}

err_t httpd_post_begin(void *connection, const char *uri,
                       const char *http_request, u16_t http_request_len,
                       int content_len, char *response_uri,
                       u16_t response_uri_len, u8_t *post_auto_wnd) {
  LWIP_UNUSED_ARG(http_request);
  LWIP_UNUSED_ARG(http_request_len);
  LWIP_UNUSED_ARG(content_len);
  LWIP_UNUSED_ARG(response_uri);
  LWIP_UNUSED_ARG(response_uri_len);
  DPRINTF("POST request for URI: %s\n", uri);
  // Handle binary chunk upload via POST
  if (strncmp(uri, "/upload_chunk.cgi", sizeof("/upload_chunk.cgi") - 1) == 0) {
    // parse token and chunk index from querystring
    const char *qs = strchr(uri, '?');
    if (qs) {
      // find token
      const char *t = strstr(qs, "token=");
      upload_ctx_t *ctx = NULL;
      if (t) {
        t += sizeof("token=") - 1;  // skip 'token='
        char buf[MNGR_HTTPD_MAX_TOKEN_LEN];
        int i = 0;
        while (*t && *t != '&' && i < (int)sizeof(buf) - 1) buf[i++] = *t++;
        buf[i] = '\0';
        ctx = find_upload_ctx(buf);
      }
      // find chunk
      int chunk = 0;
      const char *c = strstr(qs, "chunk=");
      if (c) chunk = atoi(c + (sizeof("chunk=") - 1));
      if (ctx) {
        // seek to correct offset
        FRESULT fr = f_lseek(&ctx->file, (DWORD)(chunk * UPLOAD_CHUNK_SIZE));
        if (fr != FR_OK) {
          DPRINTF("POST begin: f_lseek failed: %d\n", fr);
          return ERR_VAL;
        }
        if (!alloc_post_chunk_state(connection, ctx)) {
          DPRINTF("POST begin: no state available\n");
          return ERR_VAL;
        }
        // allow immediate receive
        *post_auto_wnd = 1;
        return ERR_OK;
      }
    }
  }
  return ERR_VAL;
}

err_t httpd_post_receive_data(void *connection, struct pbuf *p) {
  if (!p) return ERR_VAL;
  // Write binary chunk data directly to file
  post_chunk_state_t *st = find_post_chunk_state(connection);
  if (st && st->ctx) {
    UINT written;
    // p->payload may be chained; write each segment
    struct pbuf *q;
    for (q = p; q != NULL; q = q->next) {
      FRESULT fr = f_write(&st->ctx->file, q->payload, q->len, &written);
      if (fr != FR_OK || written != q->len) {
        st->had_error = true;
        DPRINTF("POST receive: write failed fr=%d written=%u len=%u\n", fr,
                (unsigned)written, (unsigned)q->len);
        pbuf_free(p);
        return ERR_VAL;
      }
    }
    pbuf_free(p);
    return ERR_OK;
  }
  // If the connection is not valid, return an error
  DPRINTF("POST data received for invalid connection\n");
  pbuf_free(p);
  return ERR_VAL;
}

void httpd_post_finished(void *connection, char *response_uri,
                         u16_t response_uri_len) {
  DPRINTF("POST finished for connection\n");
  // respond with JSON status
  post_chunk_state_t *st = find_post_chunk_state(connection);
  if (st && st->had_error) {
    strcpy(json_buff, "{\"error\":\"write failed\"}");
  } else {
    strcpy(json_buff, "{\"status\":\"chunk_ok\"}");
  }
  free_post_chunk_state(st);
  // ensure LWIP returns our json
  if (response_uri_len > 0) {
    (void)snprintf(response_uri, response_uri_len, "%s", "/json.shtml");
  }
}

/**
 * @brief Server Side Include (SSI) handler for the HTTPD server.
 *
 * This function is called when the server needs to dynamically insert content
 * into web pages using SSI tags. It handles different SSI tags and generates
 * the corresponding content to be inserted into the web page.
 *
 * @param iIndex The index of the SSI handler.
 * @param pcInsert A pointer to the buffer where the generated content should be
 * inserted.
 * @param iInsertLen The length of the buffer.
 * @param current_tag_part The current part of the SSI tag being processed (used
 * for multipart SSI tags).
 * @param next_tag_part A pointer to the next part of the SSI tag to be
 * processed (used for multipart SSI tags).
 * @return The length of the generated content.
 */
static u16_t ssi_handler(int iIndex, char *pcInsert, int iInsertLen
#if LWIP_HTTPD_SSI_MULTIPART
                         ,
                         u16_t current_tag_part, u16_t *next_tag_part
#endif /* LWIP_HTTPD_SSI_MULTIPART */
#if defined(LWIP_HTTPD_FILE_STATE) && LWIP_HTTPD_FILE_STATE
                         ,
                         void *connection_state
#endif /* LWIP_HTTPD_FILE_STATE */
) {
  DPRINTF("SSI handler called with index %d\n", iIndex);
  size_t printed;
#if defined(LWIP_HTTPD_FILE_STATE) && LWIP_HTTPD_FILE_STATE
  httpd_json_state_t *json_state = (httpd_json_state_t *)connection_state;
#endif
  switch (iIndex) {
    case 0: /* "HOMEPAGE" */
    {
      // Always to the first step of the configuration
      printed = snprintf(
          pcInsert, iInsertLen, "%s",
          "<meta http-equiv='refresh' content='0;url=/browser_home.shtml'>");
      break;
    }
    case 5: /* "SDCARDB"*/
    {
      // SD card status as boolean
      printed = snprintf(pcInsert, iInsertLen, "%s",
                         sdcard_status == SDCARD_INIT_OK ? "true" : "false");
      break;
    }
    case 7: /* JSONPLD */
    {
      // DPRINTF("SSI JSONPLD handler called with index %d\n", iIndex);
      int chunk_size = MNGR_HTTPD_SSI_JSON_CHUNK_SIZE;
      /* The offset into json based on current tag part */
      size_t offset = current_tag_part * chunk_size;
#if defined(LWIP_HTTPD_FILE_STATE) && LWIP_HTTPD_FILE_STATE
      const char *json_src = (json_state && json_state->in_use)
                                 ? json_state->json_snapshot
                                 : json_buff;
#else
      const char *json_src = json_buff;
#endif
      size_t json_len = strlen(json_src);

      /* If offset is beyond the end, we have no more data */
      if (offset >= json_len) {
        /* No more data, so no next part */
        printed = 0;
        break;
      }

      /* Calculate how many bytes remain from offset */
      size_t remain = json_len - offset;
      /* We want to send up to chunk_size bytes per part, or what's left if
       * <chunk_size */
      size_t chunk_len = (remain < chunk_size) ? remain : chunk_size;

      /* Also ensure we don't exceed iInsertLen - 1, to leave room for '\0' */
      if (chunk_len > (size_t)(iInsertLen - 1)) {
        chunk_len = iInsertLen - 1;
      }

      /* Copy that chunk into pcInsert */
      memcpy(pcInsert, &json_src[offset], chunk_len);
      pcInsert[chunk_len] = '\0'; /* null-terminate */

      printed = (u16_t)chunk_len;

      /* If there's more data after this chunk, increment next_tag_part */
      if ((offset + chunk_len) < json_len) {
        *next_tag_part = current_tag_part + 1;
      }
      break;
    }
    case 8: /* DWNLDSTS */
    {
      download_status_t status = download_getStatus();

      switch (status) {
        case DOWNLOAD_STATUS_IDLE:
        case DOWNLOAD_STATUS_COMPLETED:
          printed = snprintf(pcInsert, iInsertLen, "%s",
                             "<meta http-equiv='refresh' "
                             "content='0;url=/browser_home.shtml'>");
          break;
          break;
        case DOWNLOAD_STATUS_FAILED:
          printed = snprintf(
              pcInsert, iInsertLen,
              "<meta http-equiv='refresh' "
              "content='0;url=/"
              "error.shtml?error=%d&error_msg=Download%%20error:%%20%s'>",
              status, download_getErrorString());
          break;
          break;
        default:
          printed = snprintf(
              pcInsert, iInsertLen, "%s",
              "<meta http-equiv='refresh' content='5;url=/downloading.shtml'>");
          break;
      }
      break;
    }
    case 9: /* TITLEHDR */
    {
#if _DEBUG == 0
      printed = snprintf(pcInsert, iInsertLen, "%s (%s)", BROWSER_TITLE,
                         RELEASE_VERSION);
#else
      printed = snprintf(pcInsert, iInsertLen, "%s (%s-%s)", BROWSER_TITLE,
                         RELEASE_VERSION, RELEASE_DATE);
#endif
      break;
    }
    default: {
      // Handle other SSI tags
      DPRINTF("Unknown SSI tag index: %d\n", iIndex);
      printed = snprintf(pcInsert, iInsertLen, "Unknown SSI tag");
      break;
    }
  }
  return (u16_t)printed;
}

// The main function should be as follows:
void mngr_httpd_start(int sdcard_err) {
  // Set the SD card status based on the error code
  sdcard_status = sdcard_err;
  // Initialize the HTTP server with SSI tags and CGI handlers
  httpd_server_init(ssi_tags, LWIP_ARRAYSIZE(ssi_tags), ssi_handler,
                    cgi_handlers, LWIP_ARRAYSIZE(cgi_handlers));
}
