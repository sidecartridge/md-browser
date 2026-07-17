/**
 * File: download.c
 * Author: Diego Parrilla Santamaría
 * Date: February 2025
 * Copyright: 2025 - GOODDATA LABS
 * Description: Download files. Wrapper for HTTP and SD card.
 */

#include "download.h"

// Download
static FIL file;
static download_status_t downloadStatus = DOWNLOAD_STATUS_IDLE;
static HTTPC_REQUEST_T request = {0};
static download_url_components_t components;
static download_file_t fileUrl;
static char url[DOWNLOAD_BUFFLINE_SIZE] = {0};
static char dst_folder[DOWNLOAD_BUFFLINE_SIZE] = {0};
static char filepath[DOWNLOAD_BUFFLINE_SIZE] = {0};
static int lastHttpStatus = 0;       // Last HTTP status seen (0 = none yet)
static bool httpStatusFailed = false;  // Failure caused by a non-2xx status

// Close and remove the destination file after a failed transfer so no
// partial or bogus content (error pages, redirect stubs) is left behind.
static void cleanupFailedDownloadFile(void) {
  f_close(&file);
  if (filepath[0] != '\0') {
    f_unlink(filepath);
  }
}

// Parses a URL into its components and extracts the file name.
static int parseUrl(const char *url, download_url_components_t *components,
                    download_file_t *file) {
  if (!url || !components || !file) {
    return -1;  // Invalid arguments.
  }

  // Initialize the output structures.
  memset(components, 0, sizeof(download_url_components_t));
  memset(file, 0, sizeof(download_file_t));

  // Copy the full URL into file->url (ensure it fits)
  strncpy(file->url, url, sizeof(file->url) - 1);
  file->url[sizeof(file->url) - 1] = '\0';

  // Find the protocol separator "://"
  const char *protocolEnd = strstr(url, "://");
  if (!protocolEnd) {
    return -1;  // Invalid URL format.
  }

  size_t protocolLen = protocolEnd - url;
  if (protocolLen >= sizeof(components->protocol)) {
    return -1;  // Protocol too long.
  }
  strncpy(components->protocol, url, protocolLen);
  components->protocol[protocolLen] = '\0';

  // The host begins after "://"
  const char *hostStart = protocolEnd + 3;
  // Find the start of the URI (first slash after host_start)
  const char *uriStart = strchr(hostStart, '/');
  size_t hostLen;

  if (uriStart) {
    hostLen = uriStart - hostStart;
    if (hostLen >= sizeof(components->host)) {
      return -1;  // Host too long.
    }
    strncpy(components->host, hostStart, hostLen);
    components->host[hostLen] = '\0';

    // Copy the URI (including the leading '/')
    strncpy(components->uri, uriStart, sizeof(components->uri) - 1);
    components->uri[sizeof(components->uri) - 1] = '\0';
  } else {
    // No URI; host is the rest of the URL.
    strncpy(components->host, hostStart, sizeof(components->host) - 1);
    components->host[sizeof(components->host) - 1] = '\0';
  }

  // Extract the filename from the URI.
  // Look for the last '/' in components->uri.
  const char *lastSlash = strrchr(components->uri, '/');
  const char *filenameStart = lastSlash ? lastSlash + 1 : components->uri;

  if (filenameStart && filenameStart[0] != '\0') {
    // Copy the filename into file->filename.
    strncpy(file->filename, filenameStart, sizeof(file->filename) - 1);
    file->filename[sizeof(file->filename) - 1] = '\0';
  } else {
    // If no filename is found, you might decide to use a default name.
    strncpy(file->filename, "default.bin", sizeof(file->filename) - 1);
    file->filename[sizeof(file->filename) - 1] = '\0';
  }

  return 0;  // Success.
}

// Save body to file
static err_t httpClientReceiveFileFn(__unused void *arg,
                                     __unused struct altcp_pcb *conn,
                                     struct pbuf *ptr, err_t err) {
  // Check for null input or errors
  if (ptr == NULL) {
    DPRINTF("End of data or connection closed by the server.\n");
    downloadStatus = DOWNLOAD_STATUS_COMPLETED;
    return ERR_OK;  // Signal the connection closure
  }

  if (err != ERR_OK) {
    DPRINTF("Error receiving file: %i\n", err);
    downloadStatus = DOWNLOAD_STATUS_FAILED;
    return ERR_VAL;  // Invalid input or error occurred
  }

  // Allocate buffer to hold pbuf content
  char *buffc = malloc(ptr->tot_len);
  if (buffc == NULL) {
    DPRINTF("Error allocating memory\n");
    downloadStatus = DOWNLOAD_STATUS_FAILED;
    return ERR_MEM;  // Memory allocation failed
  }

  // Use pbuf_copy_partial to copy the pbuf content to the buffer
  pbuf_copy_partial(ptr, buffc, ptr->tot_len, 0);

  // Write the buffer to the file. File descriptor is 'file'
  FRESULT res;
  UINT bytesWritten;
  res = f_write(&file, buffc, ptr->tot_len, &bytesWritten);

  // Free the allocated memory
  free(buffc);

  // Check for file write errors
  if (res != FR_OK || bytesWritten != ptr->tot_len) {
    DPRINTF("Error writing to file: %i\n", res);
    downloadStatus = DOWNLOAD_STATUS_FAILED;
    return ERR_ABRT;  // Abort on failure
  }

  // Acknowledge that we received the data
#if FMANAGER_DOWNLOAD_HTTPS == 1
  altcp_recved(conn, ptr->tot_len);
#else
  tcp_recved(conn, ptr->tot_len);
#endif

  // Free the pbuf
  pbuf_free(ptr);

  downloadStatus = DOWNLOAD_STATUS_IN_PROGRESS;
  return ERR_OK;
}

// Function to parse headers and check Content-Length
static err_t httpClientHeaderCheckSizeFn(__unused httpc_state_t *connection,
                                         __unused void *arg, struct pbuf *hdr,
                                         u16_t hdrLen,
                                         __unused u32_t contentLen) {
  downloadStatus = DOWNLOAD_STATUS_FAILED;
  const char *contentLengthLabel = "Content-Length:";
  u16_t offset = 0;
  char *headerData = malloc(hdrLen + 1);

  if (headerData == NULL) {
    return ERR_MEM;  // Memory allocation failed
  }

  // Copy header data into a buffer for parsing
  pbuf_copy_partial(hdr, headerData, hdrLen, 0);
  headerData[hdrLen] = '\0';  // Null-terminate the string

  // Parse the HTTP status from the status line ("HTTP/1.x NNN ...").
  // lwIP delivers the headers pbuf from the first byte of the response.
  int httpStatus = 0;
  if (strncmp(headerData, "HTTP/", 5) == 0) {
    const char *statusStart = strchr(headerData, ' ');
    if (statusStart != NULL) {
      httpStatus = atoi(statusStart + 1);
    }
  }
  lastHttpStatus = httpStatus;

  // Only 2xx responses carry the requested content. Abort anything else
  // before the body is delivered, so an error page is never written to
  // the destination file. Returning non-OK makes lwIP close the
  // connection with HTTPC_RESULT_LOCAL_ABORT.
  if (httpStatus < 200 || httpStatus >= 300) {
    DPRINTF("HTTP status %d: aborting transfer before body\n", httpStatus);
    free(headerData);
    httpStatusFailed = true;
    downloadStatus = DOWNLOAD_STATUS_FAILED;
    return ERR_ABRT;
  }

  // Find the Content-Length header
  char *contentLengthStart = strstr(headerData, contentLengthLabel);
  if (contentLengthStart != NULL) {
    contentLengthStart +=
        strlen(contentLengthLabel);  // Move past "Content-Length:"

    // Skip leading spaces
    while (*contentLengthStart == ' ') {
      contentLengthStart++;
    }

    // Convert the Content-Length value to an integer
    size_t contentLength = strtoul(contentLengthStart, NULL, DEC_BASE);
  }

  free(headerData);  // Free allocated memory
  downloadStatus = DOWNLOAD_STATUS_IN_PROGRESS;
  return ERR_OK;  // Header check passed
}

static void httpClientResultCompleteFn(void *arg, httpc_result_t httpcResult,
                                       u32_t rxContentLen, u32_t srvRes,
                                       err_t err) {
  HTTPC_REQUEST_T *req = (HTTPC_REQUEST_T *)arg;
  DPRINTF("Request complete: result %d len %u server_response %u err %d\n",
          httpcResult, rxContentLen, srvRes, err);
  req->complete = true;

  // Belt and braces: even if the headers callback did not run (or missed
  // the status), a non-2xx server response is a failure.
  bool statusOk = (srvRes >= 200 && srvRes < 300);
  if (!statusOk && srvRes != 0) {
    lastHttpStatus = (int)srvRes;
    httpStatusFailed = true;
  }

  if (err == ERR_OK && statusOk &&
      downloadStatus != DOWNLOAD_STATUS_FAILED) {
    downloadStatus = DOWNLOAD_STATUS_COMPLETED;
  } else {
    downloadStatus = DOWNLOAD_STATUS_FAILED;
    // Runs in the cooperative poll context (same thread as the main
    // loop), so FatFs access is safe here.
    cleanupFailedDownloadFile();
  }
}

download_err_t download_start() {
  // Download the app binary from the URL in the app_info struct
  // The binary is saved to the SD card in the folder
  // The binary is downloaded using the HTTP client
  // The binary is saved to the SD card

  // Get the components of a url
  if (parseUrl(url, &components, &fileUrl) != 0) {
    DPRINTF("Error parsing URL\n");
    return DOWNLOAD_CANNOTPARSEURL_ERROR;
  }

  // Reset the failure latches for this attempt
  lastHttpStatus = 0;
  httpStatusFailed = false;

  // Concatane in filename the folder and filename
  char filename[DOWNLOAD_BUFFLINE_SIZE] = {0};
  snprintf(filename, sizeof(filename), "%s/%s", dst_folder, fileUrl.filename);
  snprintf(filepath, sizeof(filepath), "%s", filename);

  // Close any previously open handle
  DPRINTF("Closing any previously open file\n");
  f_close(&file);

  // Clear read-only attribute if necessary
  DPRINTF("Clearing read-only attribute, if any\n");
  f_chmod(filename, 0, AM_RDO);

  // Open file for writing or create if it doesn't exist
  DPRINTF("Opening file for writing\n");
  FRESULT res = f_open(&file, filename, FA_WRITE | FA_CREATE_ALWAYS);
  if (res == FR_LOCKED) {
    DPRINTF("File is locked. Attempting to resolve...\n");

    // Try to remove the file and create it again
    DPRINTF("Removing file and creating again\n");
    res = f_unlink(filename);
    if (res == FR_OK || res == FR_NO_FILE) {
      DPRINTF("File removed. Creating again\n");
      res = f_open(&file, filename, FA_WRITE | FA_CREATE_ALWAYS);
    }
  }

  if (res != FR_OK) {
    DPRINTF("Error opening file %s: %i\n", filename, res);
    return DOWNLOAD_CANNOTOPENFILE_ERROR;
  }

  downloadStatus = DOWNLOAD_STATUS_STARTED;

  request.url = components.uri;
  request.hostname = components.host;
  DPRINTF("HOST: %s. URI: %s\n", components.host, components.uri);
  request.headers_fn = httpClientHeaderCheckSizeFn;
  request.recv_fn = httpClientReceiveFileFn;
  request.result_fn = httpClientResultCompleteFn;
  DPRINTF("Downloading: %s\n", request.url);
#if FMANAGER_DOWNLOAD_HTTPS == 1
  request.tls_config = altcp_tls_create_config_client(NULL, 0);  // https
  DPRINTF("Download with HTTPS\n");
#else
  DPRINTF("Download with HTTP\n");
#endif
  int result = http_client_request_async(cyw43_arch_async_context(), &request);
  if (result != 0) {
    DPRINTF("Error initializing the download: %i\n", result);
    res = f_close(&file);
    if (res != FR_OK) {
      DPRINTF("Error closing file %s: %i\n", filename, res);
    }
    return DOWNLOAD_CANNOTSTARTDOWNLOAD_ERROR;
  }
  return DOWNLOAD_OK;
}

download_poll_t download_poll() {
  if (!request.complete) {
    async_context_poll(cyw43_arch_async_context());
    async_context_wait_for_work_ms(cyw43_arch_async_context(),
                                   DOWNLOAD_POLLING_INTERVAL_MS);
    // A failure latched by the callbacks must survive the polling loop.
    if (downloadStatus != DOWNLOAD_STATUS_FAILED) {
      downloadStatus = DOWNLOAD_STATUS_IN_PROGRESS;
    }
    return DOWNLOAD_POLL_CONTINUE;
  }
  if (downloadStatus == DOWNLOAD_STATUS_FAILED) {
    return DOWNLOAD_POLL_ERROR;
  }
  downloadStatus = DOWNLOAD_STATUS_COMPLETED;
  return DOWNLOAD_POLL_COMPLETED;
}

download_err_t download_finish() {
  // Close the file
  int res = f_close(&file);
  if (res != FR_OK) {
    DPRINTF("Error closing tmp file %s: %i\n", res);
    return DOWNLOAD_CANNOTCLOSEFILE_ERROR;
  }
  DPRINTF("Downloaded.\n");

#if FMANAGER_DOWNLOAD_HTTPS == 1
  altcp_tls_free_config(request.tls_config);
#endif

  if (downloadStatus != DOWNLOAD_STATUS_COMPLETED) {
    DPRINTF("Error downloading: %i\n", downloadStatus);
    return DOWNLOAD_FORCEDABORT_ERROR;
  }
  DPRINTF("File downloaded\n");

  return DOWNLOAD_OK;
}

download_status_t download_getStatus() { return downloadStatus; }

void download_setStatus(download_status_t status) { downloadStatus = status; }

// Add setter and getter for download URL and destination folder
void download_setUrl(const char *u) {
  strncpy(url, u, sizeof(url) - 1);
  url[sizeof(url) - 1] = '\0';
}

const char *download_getUrl(void) { return url; }

void download_setDstFolder(const char *d) {
  strncpy(dst_folder, d, sizeof(dst_folder) - 1);
  dst_folder[sizeof(dst_folder) - 1] = '\0';
}

const char *download_getDstFolder(void) { return dst_folder; }

const download_url_components_t *download_getUrlComponents() {
  return &components;
}

const char *download_getErrorString() {
  static char httpErrMsg[32];
  if (httpStatusFailed && lastHttpStatus != 0) {
    snprintf(httpErrMsg, sizeof(httpErrMsg), "HTTP error %d", lastHttpStatus);
    return httpErrMsg;
  }
  switch (downloadStatus) {
    case DOWNLOAD_OK:
      return "No error";
    case DOWNLOAD_BASE64_ERROR:
      return "Error decoding base64";
    case DOWNLOAD_PARSEJSON_ERROR:
      return "Error parsing JSON";
    case DOWNLOAD_PARSEMD5_ERROR:
      return "Error parsing MD5";
    case DOWNLOAD_CANNOTOPENFILE_ERROR:
      return "Cannot open file";
    case DOWNLOAD_CANNOTCLOSEFILE_ERROR:
      return "Cannot close file";
    case DOWNLOAD_FORCEDABORT_ERROR:
      return "Download aborted";
    case DOWNLOAD_CANNOTSTARTDOWNLOAD_ERROR:
      return "Cannot start download";
    case DOWNLOAD_CANNOTREADFILE_ERROR:
      return "Cannot read file";
    case DOWNLOAD_CANNOTPARSEURL_ERROR:
      return "Cannot parse URL";
    case DOWNLOAD_MD5MISMATCH_ERROR:
      return "MD5 mismatch";
    case DOWNLOAD_CANNOTRENAMEFILE_ERROR:
      return "Cannot rename file";
    case DOWNLOAD_CANNOTCREATE_CONFIG:
      return "Cannot create configuration";
    case DOWNLOAD_CANNOTDELETECONFIGSECTOR_ERROR:
      return "Cannot delete configuration sector";
    default:
      return "Unknown error";
  }
}
