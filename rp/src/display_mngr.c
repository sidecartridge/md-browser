/**
 * File: display_mngr.c
 * Author: Diego Parrilla Santamaría
 * Date: December 2024
 * Copyright: 2024 - GOODDATA LABS SL
 * Description: Functions for display in manager mode.
 */

#include "display_mngr.h"

// Static assert to ensure buffer size fits within uint32_t
_Static_assert(DISPLAY_BUFFER_SIZE <= UINT32_MAX,
               "Buffer size exceeds allowed limits");

// Draw graphics into the buffer
void draw_connection_scr(const uint8_t qrcode_url[], const char *ssid,
                         const char *url1, const char *url2,
                         uint8_t wifi_status) {
  // // Clear the buffer first
  u8g2_ClearBuffer(display_getU8g2Ref());

  display_drawQr(qrcode_url, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0,
                 DISPLAY_QR_BORDER, DISPLAY_MNGR_QR_SCALE);

  // Connection info
  char ssid_str[40] = {0};
  snprintf(ssid_str, sizeof(ssid_str), "SSID: %s", ssid);

  // Use Amstrad CPC font!
  u8g2_SetFont(display_getU8g2Ref(), u8g2_font_amstrad_cpc_extended_8f);

  // Display inverted Title
  u8g2_DrawBox(display_getU8g2Ref(), 0, 0, DISPLAY_WIDTH, 8);
  u8g2_SetDrawColor(display_getU8g2Ref(), 0);
  char title_str[40] = {0};
  snprintf(title_str, sizeof(title_str), "%s - %s", BROWSER_TITLE,
           RELEASE_VERSION);
  u8g2_DrawStr(display_getU8g2Ref(),
               LEFT_PADDING_FOR_CENTER(title_str, DISPLAY_TILES_WIDTH) * 8, 8,
               title_str);
  u8g2_SetDrawColor(display_getU8g2Ref(), 1);

  // Connection info
  u8g2_DrawStr(display_getU8g2Ref(),
               LEFT_PADDING_FOR_CENTER(ssid_str, DISPLAY_TILES_WIDTH) * 8, 40,
               ssid_str);

  // Wifi status
  display_mngr_wifi_change_status(wifi_status, url1, url2, NULL);

  // Product info
  display_drawProductInfo();

  // ByPass message
  u8g2_DrawStr(display_getU8g2Ref(),
               LEFT_PADDING_FOR_CENTER(DISPLAY_MANAGER_BYPASS_MESSAGE, 68) * 5,
               DISPLAY_HEIGHT - 9, DISPLAY_MANAGER_BYPASS_MESSAGE);
}

// Change the status in the buffer
void display_mngr_change_status(uint8_t status, const char *details) {
  // Use 8x8 font
  u8g2_SetFont(display_getU8g2Ref(), u8g2_font_amstrad_cpc_extended_8f);

  // Status
  char status_str[40] = {0};
  switch (status) {
    case 0:
      snprintf(status_str, sizeof(status_str), "Connecting to WIFI...");
      break;
    case 1:
      snprintf(status_str, sizeof(status_str), "      Connected!     ");
      break;
    case 2:
      snprintf(status_str, sizeof(status_str),
               details != NULL ? details : "   Connection Error! ");
      break;
    default:
      snprintf(status_str, sizeof(status_str), "   Launching App...  ");
      break;
  }

  u8g2_DrawStr(display_getU8g2Ref(),
               LEFT_PADDING_FOR_CENTER(status_str, DISPLAY_TILES_WIDTH) * 8,
               DISPLAY_HEIGHT - 24, status_str);
}

// Change the wifi status in the buffer
void display_mngr_wifi_change_status(uint8_t wifi_status, const char *url1,
                                     const char *url2, const char *details) {
  // Wifi status
  display_mngr_change_status(wifi_status, details);

  if (wifi_status == 1) {
    u8g2_SetDrawColor(display_getU8g2Ref(), 0);
    u8g2_DrawBox(display_getU8g2Ref(), 0, DISPLAY_HEIGHT - -16, DISPLAY_WIDTH,
                 8);
    u8g2_SetDrawColor(display_getU8g2Ref(), 1);

    // URL
    char url_str[72] = {0};
    snprintf(url_str, sizeof(url_str), "%s or %s", url1, url2);
    u8g2_SetFont(display_getU8g2Ref(), u8g2_font_squeezed_b7_tr);
    u8g2_DrawStr(display_getU8g2Ref(),
                 LEFT_PADDING_FOR_CENTER(DISPLAY_BYPASS_MESSAGE, 68) * 5, 30,
                 url_str);
  }
  if (wifi_status == 2) {
    // Error!
    u8g2_SetFont(display_getU8g2Ref(), u8g2_font_squeezed_b7_tr);
    u8g2_DrawStr(
        display_getU8g2Ref(),
        LEFT_PADDING_FOR_CENTER(DISPLAY_MNGR_SELECT_RESET_MESSAGE, 68) * 5,
        DISPLAY_HEIGHT - 9, DISPLAY_MNGR_SELECT_RESET_MESSAGE);
  } else {
    // Clear the error message
    // u8g2_DrawStr(display_getU8g2Ref(),
    //              LEFT_PADDING_FOR_CENTER(DISPLAY_MNGR_EMPTY_MESSAGE, 68) * 5,
    //              DISPLAY_HEIGHT - 8, DISPLAY_MNGR_EMPTY_MESSAGE);
  }
}

// Change the USB status in the buffer
void display_mngr_usb_change_status(bool usb_connected) {
  u8g2_SetDrawColor(display_getU8g2Ref(), 0);
  u8g2_DrawBox(display_getU8g2Ref(), 0, DISPLAY_TILE_HEIGHT * 2, DISPLAY_WIDTH,
               8);

  u8g2_SetDrawColor(display_getU8g2Ref(), 1);

  if (usb_connected) {
    // Erase the message screen
    // Use Amstrad CPC font!
    u8g2_SetFont(display_getU8g2Ref(), u8g2_font_amstrad_cpc_extended_8f);
    // Connection info
    const char *usb_str = "USB Mass Storage Connected";
    u8g2_DrawStr(display_getU8g2Ref(),
                 LEFT_PADDING_FOR_CENTER(usb_str, DISPLAY_TILES_WIDTH) *
                     DISPLAY_TILE_WIDHT,
                 DISPLAY_TILE_HEIGHT * 3, usb_str);
  }
}

// The main function should be as follows:
void display_mngr_start(const char *ssid, const char *url1, const char *url2) {
  size_t buffer_size = DISPLAY_BUFFER_SIZE;  // Safe usage
  (void)buffer_size;  // To avoid unused variable warning if not used elsewhere

  // Initialize the u8g2 library for a custom buffer
  display_setupU8g2();

  // Create the QR codes
  uint8_t qrcode_url[DISPLAY_QR_BUFFER_LEN_MAX];
  display_createQr(qrcode_url, url1);

  // Set the flag to NOT-RESET the computer
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_NOP);

  draw_connection_scr(qrcode_url, ssid, url1, url2, 0);
  display_refresh();

  DPRINTF("Exiting fabric display\n");
}
