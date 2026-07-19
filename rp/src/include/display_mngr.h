/**
 * File: display_mngr.h
 * Author: Diego Parrilla Santamaría
 * Date: December 2024
 * Copyright: 2024 - GOODDATA LABS SL
 * Description: Header file for the mnaager mode diplay functions.
 */

#ifndef DISPLAY_MNGR_H
#define DISPLAY_MNGR_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <u8g2.h>

#include "constants.h"
#include "debug.h"
#include "display.h"
#include "hardware/dma.h"
#include "memfunc.h"
#include "network.h"
#include "qrcodegen.h"

#define DISPLAY_MNGR_QR_SCALE 5

#define DISPLAY_MNGR_SELECT_RESET_MESSAGE \
  "If can't connect to your WiFi, press SELECT for 10 seconds to restart."

#define DISPLAY_MNGR_EMPTY_MESSAGE                                             \
  "                                                                          " \
  "      "

// For Atari ST display
#ifdef DISPLAY_ATARIST
#define DISPLAY_MANAGER_BYPASS_MESSAGE \
  "Press ESC to return to Booster. Other keys to boot from GEMDOS."
#endif

void display_mngr_start(const char *ssid, const char *url1, const char *url2);
void display_mngr_no_sdcard(void);
void display_mngr_wifi_change_status(uint8_t wifi_status, const char *url1,
                                     const char *url2, const char *details);
void display_mngr_change_status(uint8_t status, const char *details);
void display_mngr_usb_change_status(bool usb_connected);

#endif  // DISPLAY_MNGR_H
