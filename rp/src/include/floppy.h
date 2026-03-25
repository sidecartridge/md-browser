/**
 * File: floppy.h
 * Description: Atari ST / MSA disk image helpers reused from the floppy
 * project.
 */

#ifndef FLOPPY_H
#define FLOPPY_H

#include <stddef.h>
#include <stdbool.h>

#include "ff.h"

void floppy_removeMSAExtension(char *filename);

FRESULT floppy_createSTImage(const char *folder, char *stFilename, int nTracks,
                             int nSectors, int nSides, const char *volLavel,
                             bool overwrite);
FRESULT floppy_MSA2ST(const char *folder, char *msaFilename, char *stFilename,
                      bool overwrite);
FRESULT floppy_ST2MSA(const char *folder, char *stFilename, char *msaFilename,
                      bool overwrite);

#endif  // FLOPPY_H
