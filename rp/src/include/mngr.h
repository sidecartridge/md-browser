/**
 * File: mngr.h
 * Author: Diego Parrilla Santamaría
 * Date: December 2024
 * Copyright: 2024 - GOODDATA LABS SL
 * Description: Header file for the generic manager mode.
 */

#ifndef MNGR_H
#define MNGR_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "blink.h"
#include "constants.h"
#include "debug.h"
#include "display.h"
#include "display_mngr.h"
#include "gconfig.h"
#include "httpc/httpc.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/httpd.h"
#include "memfunc.h"
#include "mngr_httpd.h"
#include "network.h"
#include "pico/async_context.h"
#include "pico/multicore.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "reset.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "tprotocol.h"
#include "usb_mass.h"

#define ADDRESS_HIGH_BIT 0x8000  // High bit of the address

#ifndef ROM3_GPIO
#define ROM3_GPIO 26
#endif

#ifndef ROM4_GPIO
#define ROM4_GPIO 22
#endif

// Use the highest 4K of the shared memory for the terminal commands
#define TERM_RANDOM_TOKEN_OFFSET \
  0xF000  // Random token offset in the shared memory
#define TERM_RANDON_TOKEN_SEED_OFFSET \
  (TERM_RANDOM_TOKEN_OFFSET +         \
   4)  // Random token seed offset in the shared memory: 0xF004

// Size of the shared variables of the shared functions
#define SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE \
  16  // Leave a gap for the shared variables of the shared functions

// The shared variables are located in the + 0x200 offset
#define TERM_SHARED_VARIABLES_OFFSET        \
  (TERM_RANDOM_TOKEN_OFFSET +               \
   (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE * \
    4))  // Shared variables offset in the shared memory: 0xF000 + (16 * 4)

// Shared variables for common use. Must be set in the init function
#define TERM_HARDWARE_TYPE (0)     // Hardware type. 0xF200
#define TERM_HARDWARE_VERSION (1)  // Hardware version.  0xF204

// App commands for the terminal
#define APP_TERMINAL 0x00  // The terminal app

// App terminal commands
#define APP_BOOSTER_START 0x00  // Launch the booster app

#define DISPLAY_COMMAND_BOOSTER 0x3  // Enter booster mode

#define TERM_PARAMETERS_MAX_SIZE 20  // Maximum size of the parameters

void mngr_dma_irq_handler_lookup(void);

int mngr_init(void);
void mngr_loop();

#endif  // MNGR_H
