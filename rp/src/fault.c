/**
 * File: fault.c
 * Author: Diego Parrilla Santamaría
 * Date: July 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Minimal hard fault reporter. Overrides the SDK's weak
 * hardfault vector, prints the stacked exception frame over stdio and
 * halts, so a crash leaves a diagnosable trace on the serial console
 * instead of a silent reboot.
 */

#include <stdio.h>

#include "pico/stdlib.h"

typedef struct {
  uint32_t r0, r1, r2, r3, r12, lr, pc, psr;
} fault_frame_t;

void __attribute__((used)) fault_dump(fault_frame_t *frame) {
  printf("\n*** HARD FAULT on core %d ***\n", get_core_num());
  printf("PC=%08lx LR=%08lx PSR=%08lx\n", frame->pc, frame->lr, frame->psr);
  printf("R0=%08lx R1=%08lx R2=%08lx R3=%08lx R12=%08lx\n", frame->r0,
         frame->r1, frame->r2, frame->r3, frame->r12);
  printf("frame at %08lx\n", (uint32_t)frame);
  while (true) {
    tight_loop_contents();  // halt here so the dump stays visible
  }
}

// Select MSP/PSP per EXC_RETURN and hand the stacked frame to fault_dump.
void __attribute__((naked)) isr_hardfault(void) {
  __asm volatile(
      "movs r0, #4      \n"
      "mov  r1, lr      \n"
      "tst  r0, r1      \n"
      "beq  1f          \n"
      "mrs  r0, psp     \n"
      "b    2f          \n"
      "1: mrs r0, msp   \n"
      "2: ldr r1, =fault_dump \n"
      "bx   r1          \n");
}
