/**
 * File: floppy.c
 * Description: Atari ST / MSA disk image helpers reused from md-drives-emulator
 * and Hatari hmsa.
 */

#include "include/floppy.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "include/sdcard.h"

#ifndef ST_IMAGE_MAX_PATH_LEN
#define ST_IMAGE_MAX_PATH_LEN 512
#endif

#ifndef ST_IMAGE_GEMDOS_FILE_ATTRIB_VOLUME_LABEL
#define ST_IMAGE_GEMDOS_FILE_ATTRIB_VOLUME_LABEL 8
#endif

#ifndef ST_IMAGE_SPF_MAX
#define ST_IMAGE_SPF_MAX 9
#endif

#ifndef ST_IMAGE_MAX_TRACK_BYTES
#define ST_IMAGE_MAX_TRACK_BYTES (36u * NUM_BYTES_PER_SECTOR)
#endif

typedef struct {
  uint16_t id;
  uint16_t sectors_per_track;
  uint16_t sides_minus_one;
  uint16_t starting_track;
  uint16_t ending_track;
} msa_header_t;

static FRESULT checkDiskSpace(const char *folder, uint32_t nDiskSize) {
  DWORD fre_clust = 0;
  FATFS *fs = NULL;
  FRESULT fr = f_getfree(folder, &fre_clust, &fs);
  if (fr != FR_OK) return fr;

  uint64_t freeBytes = (uint64_t)fre_clust * fs->csize * NUM_BYTES_PER_SECTOR;
  return ((uint64_t)nDiskSize > freeBytes) ? FR_DENIED : FR_OK;
}

static inline void writeShortLE(void *addr, uint16_t val) {
  uint8_t *p = (uint8_t *)addr;
  p[0] = (uint8_t)val;
  p[1] = (uint8_t)(val >> 8);
}

static inline void writeShortBE(void *addr, uint16_t val) {
  uint8_t *p = (uint8_t *)addr;
  p[0] = (uint8_t)(val >> 8);
  p[1] = (uint8_t)val;
}

static inline uint16_t readShortBE(const uint8_t *addr) {
  return (uint16_t)(((uint16_t)addr[0] << 8) | (uint16_t)addr[1]);
}

static FRESULT buildPath(const char *folder, const char *filename, char *path,
                         size_t path_len) {
  if (!folder || !filename || !path || path_len == 0) {
    return FR_INVALID_PARAMETER;
  }

  int written = snprintf(path, path_len, "%s/%s", folder, filename);
  if (written < 0 || (size_t)written >= path_len) {
    return FR_INVALID_NAME;
  }

  return FR_OK;
}

static FRESULT writeExact(FIL *file, const void *buffer, UINT len) {
  UINT written = 0;
  FRESULT fr = f_write(file, buffer, len, &written);
  if (fr != FR_OK) return fr;
  return (written == len) ? FR_OK : FR_DISK_ERR;
}

static FRESULT readExact(FIL *file, void *buffer, UINT len) {
  UINT read = 0;
  FRESULT fr = f_read(file, buffer, len, &read);
  if (fr != FR_OK) return fr;
  return (read == len) ? FR_OK : FR_DISK_ERR;
}

static char *allocPathBuffer(void) {
  return calloc(1, ST_IMAGE_MAX_PATH_LEN);
}

static bool hasSuffixCI(const char *value, const char *suffix) {
  size_t value_len = 0;
  size_t suffix_len = 0;

  if (!value || !suffix) return false;

  value_len = strlen(value);
  suffix_len = strlen(suffix);
  if (value_len < suffix_len) return false;

  value += value_len - suffix_len;
  for (size_t i = 0; i < suffix_len; i++) {
    if (tolower((unsigned char)value[i]) !=
        tolower((unsigned char)suffix[i])) {
      return false;
    }
  }

  return true;
}

void floppy_removeMSAExtension(char *filename) {
  size_t len = 0;

  if (!filename) return;

  len = strlen(filename);
  if (len >= 4 && hasSuffixCI(filename, ".msa")) {
    filename[len - 4] = '\0';
  }
}

static void floppy_doubleCheckFormat(uint32_t disk_size, uint16_t *pnSides,
                                     uint16_t *pnSectorsPerTrack) {
  static const uint16_t kFallbackSpt[] = {9, 10, 11, 12, 18, 36};
  uint32_t totalSectors = disk_size / NUM_BYTES_PER_SECTOR;
  uint16_t sectors_per_track = 0;

  if (!pnSides || !pnSectorsPerTrack) return;

  *pnSides = (disk_size < (500u * 1024u)) ? 1u : 2u;

  sectors_per_track = *pnSectorsPerTrack;
  if (sectors_per_track == 0) sectors_per_track = 9;

  if ((totalSectors % sectors_per_track) == 0) {
    *pnSectorsPerTrack = sectors_per_track;
    return;
  }

  for (size_t i = 0; i < sizeof(kFallbackSpt) / sizeof(kFallbackSpt[0]); i++) {
    if ((totalSectors % kFallbackSpt[i]) == 0) {
      *pnSectorsPerTrack = kFallbackSpt[i];
      return;
    }
  }
}

static void floppy_findDiskDetails(const uint8_t *boot_sector,
                                   uint32_t image_bytes,
                                   uint16_t *pnSectorsPerTrack,
                                   uint16_t *pnSides) {
  uint16_t sectors_per_track = 0;
  uint16_t sides = 0;
  uint16_t total_sectors = 0;

  if (!boot_sector) return;

  sectors_per_track = (uint16_t)(boot_sector[24] | (boot_sector[25] << 8));
  sides = (uint16_t)(boot_sector[26] | (boot_sector[27] << 8));
  total_sectors = (uint16_t)(boot_sector[19] | (boot_sector[20] << 8));

  if (total_sectors != image_bytes / NUM_BYTES_PER_SECTOR || sides == 0 ||
      sides > 2 || sectors_per_track == 0) {
    floppy_doubleCheckFormat(image_bytes, &sides, &sectors_per_track);
  }

  if (pnSectorsPerTrack) *pnSectorsPerTrack = sectors_per_track;
  if (pnSides) *pnSides = sides;
}

static int msa_findRunOfBytes(const uint8_t *buffer, int bytes_in_buffer) {
  uint8_t scanned_byte = 0;
  bool marker = false;
  int run = 0;

  if (!buffer || bytes_in_buffer <= 0) return 0;

  marker = (*buffer == 0xE5);
  if (bytes_in_buffer < 2) {
    return (bytes_in_buffer == 1 && marker) ? 1 : 0;
  }

  scanned_byte = *buffer++;
  run = 1;
  for (int i = 1; i < bytes_in_buffer; i++) {
    if (*buffer++ == scanned_byte) {
      run++;
    } else {
      break;
    }
  }

  if (run < 4 && !marker) run = 0;
  return run;
}

static size_t msa_measureEncodedTrack(const uint8_t *buffer, uint16_t bytes) {
  size_t encoded_size = 0;
  int bytes_left = bytes;

  while (bytes_left > 0) {
    int run = msa_findRunOfBytes(buffer, bytes_left);
    if (run == 0) {
      encoded_size++;
      buffer++;
      bytes_left--;
    } else {
      encoded_size += 4;
      buffer += run;
      bytes_left -= run;
    }

    if (encoded_size >= bytes) return bytes;
  }

  return encoded_size;
}

static bool msa_encodeTrack(const uint8_t *src, uint16_t bytes, uint8_t *dst,
                            size_t dst_len, uint16_t *encoded_len) {
  size_t out_pos = 0;
  int bytes_left = bytes;

  if (!src || !dst || !encoded_len || dst_len < bytes) return false;

  while (bytes_left > 0) {
    int run = msa_findRunOfBytes(src, bytes_left);
    if (run == 0) {
      if (out_pos + 1 > dst_len) return false;
      dst[out_pos++] = *src++;
      bytes_left--;
      continue;
    }

    if (out_pos + 4 > dst_len) return false;
    dst[out_pos++] = 0xE5;
    dst[out_pos++] = *src;
    writeShortBE(dst + out_pos, (uint16_t)run);
    out_pos += 2;
    src += run;
    bytes_left -= run;
  }

  if (out_pos >= bytes) return false;
  *encoded_len = (uint16_t)out_pos;
  return true;
}

FRESULT floppy_createSTImage(const char *folder, char *stFilename, int nTracks,
                             int nSectors, int nSides, const char *volLavel,
                             bool overwrite) {
  uint8_t *pDiskHeader = NULL;
  uint32_t nDiskSize = (uint32_t)nTracks * (uint32_t)nSectors *
                       (uint32_t)nSides * NUM_BYTES_PER_SECTOR;
  uint32_t nHeaderSize =
      2u * (1u + (uint32_t)ST_IMAGE_SPF_MAX) * NUM_BYTES_PER_SECTOR;
  uint32_t nDiskSizeNoHeader = nDiskSize - nHeaderSize;
  uint16_t SPC = 0;
  uint16_t nDir = 0;
  uint16_t MediaByte = 0;
  uint16_t SPF = 0;
  uint16_t LabelSize = 0;
  uint8_t *pDirStart = NULL;

  FRESULT fr = FR_OK;
  FIL dest_file;
  bool dest_open = false;
  char *dest_path = NULL;
  BYTE *zeroBuff = NULL;

  if (!folder || !stFilename || stFilename[0] == '\0') {
    return FR_INVALID_PARAMETER;
  }

  DPRINTF("Checking folder %s\n", folder);
  if (f_stat(folder, NULL) != FR_OK) {
    DPRINTF("Folder %s not found!\n", folder);
    return FR_NO_PATH;
  }

  fr = checkDiskSpace(folder, nDiskSize);
  if (fr != FR_OK) {
    DPRINTF("Not enough space in the SD card!\n");
    return fr;
  }

  dest_path = allocPathBuffer();
  zeroBuff = malloc(NUM_BYTES_PER_SECTOR);
  if (!dest_path || !zeroBuff) {
    free(zeroBuff);
    free(dest_path);
    return FR_NOT_ENOUGH_CORE;
  }

  fr = buildPath(folder, stFilename, dest_path, ST_IMAGE_MAX_PATH_LEN);
  if (fr != FR_OK) goto cleanup;

  DPRINTF("DEST PATH: %s\n", dest_path);

  fr = f_stat(dest_path, NULL);
  if (fr == FR_OK && !overwrite) {
    DPRINTF("Destination file exists and overwrite is false\n");
    fr = FR_EXIST;
    goto cleanup;
  }

  if (nSectors >= 18) nSides = 2;

  pDiskHeader = malloc(nHeaderSize);
  if (!pDiskHeader) {
    DPRINTF("Error while creating blank disk image\n");
    fr = FR_NOT_ENOUGH_CORE;
    goto cleanup;
  }
  memset(pDiskHeader, 0, nHeaderSize);

  pDiskHeader[0] = 0xE9;
  memset(pDiskHeader + 2, 0x4e, 6);

  writeShortLE(pDiskHeader + 8, (uint16_t)rand());
  pDiskHeader[10] = (uint8_t)rand();

  writeShortLE(pDiskHeader + 11, NUM_BYTES_PER_SECTOR);

  if ((nTracks == 40) && (nSides == 1)) {
    SPC = 1;
  } else {
    SPC = 2;
  }
  pDiskHeader[13] = (uint8_t)SPC;

  writeShortLE(pDiskHeader + 14, 1);
  pDiskHeader[16] = 2;

  if (SPC == 1) {
    nDir = 64;
  } else if (nSectors < 18) {
    nDir = 112;
  } else {
    nDir = 224;
  }
  writeShortLE(pDiskHeader + 17, nDir);

  writeShortLE(pDiskHeader + 19, (uint16_t)(nTracks * nSectors * nSides));

  if (nSectors >= 18) {
    MediaByte = 0xF0;
  } else {
    MediaByte = (nTracks <= 42) ? 0xFC : 0xF8;
    if (nSides == 2) MediaByte |= 0x01;
  }
  pDiskHeader[21] = (uint8_t)MediaByte;

  if (nSectors >= 18) {
    SPF = ST_IMAGE_SPF_MAX;
  } else if (nTracks >= 80) {
    SPF = 5;
  } else {
    SPF = 2;
  }
  writeShortLE(pDiskHeader + 22, SPF);

  writeShortLE(pDiskHeader + 24, (uint16_t)nSectors);
  writeShortLE(pDiskHeader + 26, (uint16_t)nSides);
  writeShortLE(pDiskHeader + 28, 0);

  pDiskHeader[NUM_BYTES_PER_SECTOR] = (uint8_t)MediaByte;
  pDiskHeader[NUM_BYTES_PER_SECTOR + 1] = 0xFF;
  pDiskHeader[NUM_BYTES_PER_SECTOR + 2] = 0xFF;
  pDiskHeader[NUM_BYTES_PER_SECTOR + SPF * NUM_BYTES_PER_SECTOR] =
      (uint8_t)MediaByte;
  pDiskHeader[NUM_BYTES_PER_SECTOR + SPF * NUM_BYTES_PER_SECTOR + 1] = 0xFF;
  pDiskHeader[NUM_BYTES_PER_SECTOR + SPF * NUM_BYTES_PER_SECTOR + 2] = 0xFF;

  if (volLavel != NULL) {
    pDirStart = pDiskHeader + (1 + SPF * 2) * NUM_BYTES_PER_SECTOR;
    memset(pDirStart, ' ', 11);
    LabelSize = (uint16_t)strlen(volLavel);
    memcpy(pDirStart, volLavel, LabelSize <= 11 ? LabelSize : 11);
    pDirStart[11] = ST_IMAGE_GEMDOS_FILE_ATTRIB_VOLUME_LABEL;
  }

  fr = f_open(&dest_file, dest_path, overwrite ? (FA_WRITE | FA_CREATE_ALWAYS)
                                               : (FA_WRITE | FA_CREATE_NEW));
  if (fr != FR_OK) {
    fr = (fr == FR_EXIST) ? FR_EXIST : FR_NO_FILE;
    goto cleanup;
  }
  dest_open = true;

  fr = writeExact(&dest_file, pDiskHeader, (UINT)nHeaderSize);
  if (fr != FR_OK) goto cleanup;

  memset(zeroBuff, 0, NUM_BYTES_PER_SECTOR);
  while (nDiskSizeNoHeader > 0) {
    UINT toWrite = NUM_BYTES_PER_SECTOR;
    if (nDiskSizeNoHeader < toWrite) toWrite = (UINT)nDiskSizeNoHeader;

    fr = writeExact(&dest_file, zeroBuff, toWrite);
    if (fr != FR_OK) goto cleanup;
    nDiskSizeNoHeader -= toWrite;
  }

cleanup:
  if (dest_open) {
    FRESULT close_fr = f_close(&dest_file);
    if (fr == FR_OK) fr = close_fr;
  }
  free(pDiskHeader);
  free(zeroBuff);
  free(dest_path);
  return fr;
}

FRESULT floppy_MSA2ST(const char *folder, char *msaFilename, char *stFilename,
                      bool overwrite) {
  FRESULT fr = FR_OK;
  FIL src_file;
  FIL dest_file;
  bool src_open = false;
  bool dest_open = false;
  char *src_path = NULL;
  char *dest_path = NULL;
  uint8_t header_buf[sizeof(msa_header_t)];
  uint8_t length_buf[sizeof(uint16_t)];
  uint8_t *encoded_track = NULL;
  uint8_t *write_buffer = NULL;
  uint16_t sectors_per_track = 0;
  uint16_t sides_minus_one = 0;
  uint16_t starting_track = 0;
  uint16_t ending_track = 0;
  uint16_t sides = 0;
  uint16_t track_count = 0;
  uint32_t track_bytes = 0;
  uint32_t dest_size = 0;

  if (!folder || !msaFilename || !stFilename || msaFilename[0] == '\0' ||
      stFilename[0] == '\0') {
    return FR_INVALID_PARAMETER;
  }

  if (f_stat(folder, NULL) != FR_OK) return FR_NO_PATH;

  src_path = allocPathBuffer();
  dest_path = allocPathBuffer();
  if (!src_path || !dest_path) {
    free(dest_path);
    free(src_path);
    return FR_NOT_ENOUGH_CORE;
  }

  fr = buildPath(folder, msaFilename, src_path, ST_IMAGE_MAX_PATH_LEN);
  if (fr != FR_OK) goto cleanup;
  fr = buildPath(folder, stFilename, dest_path, ST_IMAGE_MAX_PATH_LEN);
  if (fr != FR_OK) goto cleanup;

  fr = f_stat(dest_path, NULL);
  if (fr == FR_OK && !overwrite) {
    fr = FR_EXIST;
    goto cleanup;
  }

  fr = f_open(&src_file, src_path, FA_READ);
  if (fr != FR_OK) {
    fr = FR_NO_FILE;
    goto cleanup;
  }
  src_open = true;

  fr = readExact(&src_file, header_buf, sizeof(header_buf));
  if (fr != FR_OK) goto cleanup;

  sectors_per_track = readShortBE(header_buf + 2);
  sides_minus_one = readShortBE(header_buf + 4);
  starting_track = readShortBE(header_buf + 6);
  ending_track = readShortBE(header_buf + 8);

  if (readShortBE(header_buf) != 0x0E0F || ending_track > 86 ||
      starting_track > ending_track || sectors_per_track == 0 ||
      sectors_per_track > 56 || sides_minus_one > 1) {
    fr = FR_INT_ERR;
    goto cleanup;
  }

  sides = (uint16_t)(sides_minus_one + 1);
  track_count = (uint16_t)(ending_track - starting_track + 1);
  track_bytes = (uint32_t)sectors_per_track * NUM_BYTES_PER_SECTOR;
  if (track_bytes == 0 || track_bytes > ST_IMAGE_MAX_TRACK_BYTES) {
    fr = FR_INT_ERR;
    goto cleanup;
  }
  dest_size = (uint32_t)track_count * (uint32_t)sides * track_bytes;

  fr = checkDiskSpace(folder, dest_size);
  if (fr != FR_OK) goto cleanup;

  encoded_track = malloc(track_bytes);
  write_buffer = malloc(NUM_BYTES_PER_SECTOR);
  if (!encoded_track || !write_buffer) {
    fr = FR_NOT_ENOUGH_CORE;
    goto cleanup;
  }

  fr = f_open(&dest_file, dest_path, overwrite ? (FA_WRITE | FA_CREATE_ALWAYS)
                                               : (FA_WRITE | FA_CREATE_NEW));
  if (fr != FR_OK) {
    fr = (fr == FR_EXIST) ? FR_EXIST : FR_NO_FILE;
    goto cleanup;
  }
  dest_open = true;

  for (uint16_t track = 0; track < track_count && fr == FR_OK; track++) {
    for (uint16_t side = 0; side < sides; side++) {
      uint16_t encoded_len = 0;

      fr = readExact(&src_file, length_buf, sizeof(length_buf));
      if (fr != FR_OK) goto cleanup;
      encoded_len = readShortBE(length_buf);

      if (encoded_len > track_bytes) {
        fr = FR_INT_ERR;
        goto cleanup;
      }

      fr = readExact(&src_file, encoded_track, encoded_len);
      if (fr != FR_OK) goto cleanup;

      if (encoded_len == track_bytes) {
        fr = writeExact(&dest_file, encoded_track, encoded_len);
        if (fr != FR_OK) goto cleanup;
        continue;
      }

      size_t in_pos = 0;
      UINT buffered = 0;
      uint32_t bytes_remaining = track_bytes;
      while (in_pos < encoded_len && bytes_remaining > 0) {
        uint8_t byte = encoded_track[in_pos++];
        if (byte != 0xE5) {
          write_buffer[buffered++] = byte;
          bytes_remaining--;
          if (buffered == NUM_BYTES_PER_SECTOR) {
            fr = writeExact(&dest_file, write_buffer, buffered);
            if (fr != FR_OK) goto cleanup;
            buffered = 0;
          }
          continue;
        }

        if (in_pos + 3 > encoded_len) {
          fr = FR_INT_ERR;
          goto cleanup;
        }

        uint8_t value = encoded_track[in_pos++];
        uint16_t run_len = readShortBE(encoded_track + in_pos);
        in_pos += 2;
        if ((uint32_t)run_len > bytes_remaining) {
          fr = FR_INT_ERR;
          goto cleanup;
        }

        while (run_len > 0) {
          UINT chunk = (UINT)(NUM_BYTES_PER_SECTOR - buffered);
          if (chunk > run_len) chunk = run_len;
          memset(write_buffer + buffered, value, chunk);
          buffered += chunk;
          run_len -= (uint16_t)chunk;
          bytes_remaining -= chunk;

          if (buffered == NUM_BYTES_PER_SECTOR) {
            fr = writeExact(&dest_file, write_buffer, buffered);
            if (fr != FR_OK) goto cleanup;
            buffered = 0;
          }
        }
      }

      if (bytes_remaining != 0) {
        fr = FR_INT_ERR;
        goto cleanup;
      }

      if (buffered > 0) {
        fr = writeExact(&dest_file, write_buffer, buffered);
        if (fr != FR_OK) goto cleanup;
      }
    }
  }

cleanup:
  if (dest_open) {
    FRESULT close_fr = f_close(&dest_file);
    if (fr == FR_OK) fr = close_fr;
  }
  if (src_open) {
    FRESULT close_fr = f_close(&src_file);
    if (fr == FR_OK) fr = close_fr;
  }
  free(write_buffer);
  free(encoded_track);
  free(dest_path);
  free(src_path);
  return fr;
}

FRESULT floppy_ST2MSA(const char *folder, char *stFilename, char *msaFilename,
                      bool overwrite) {
  FRESULT fr = FR_OK;
  FIL src_file;
  FIL dest_file;
  bool src_open = false;
  bool dest_open = false;
  char *src_path = NULL;
  char *dest_path = NULL;
  uint8_t *boot_sector = NULL;
  uint8_t header_buf[sizeof(msa_header_t)];
  uint8_t length_buf[sizeof(uint16_t)];
  uint8_t *track_buffer = NULL;
  uint8_t *encoded_track = NULL;
  FSIZE_t source_size_fs = 0;
  uint32_t source_size = 0;
  uint32_t max_dest_size = 0;
  uint32_t track_bytes = 0;
  uint16_t sectors_per_track = 0;
  uint16_t sides = 0;
  uint16_t tracks = 0;

  if (!folder || !stFilename || !msaFilename || stFilename[0] == '\0' ||
      msaFilename[0] == '\0') {
    return FR_INVALID_PARAMETER;
  }

  fr = f_stat(folder, NULL);
  if (fr != FR_OK) {
    return FR_NO_PATH;
  }

  src_path = allocPathBuffer();
  dest_path = allocPathBuffer();
  boot_sector = malloc(NUM_BYTES_PER_SECTOR);
  if (!src_path || !dest_path || !boot_sector) {
    free(boot_sector);
    free(dest_path);
    free(src_path);
    return FR_NOT_ENOUGH_CORE;
  }

  fr = buildPath(folder, stFilename, src_path, ST_IMAGE_MAX_PATH_LEN);
  if (fr != FR_OK) goto cleanup;
  fr = buildPath(folder, msaFilename, dest_path, ST_IMAGE_MAX_PATH_LEN);
  if (fr != FR_OK) goto cleanup;

  fr = f_stat(dest_path, NULL);
  if (fr == FR_OK && !overwrite) {
    fr = FR_EXIST;
    goto cleanup;
  }

  fr = f_open(&src_file, src_path, FA_READ);
  if (fr != FR_OK) {
    fr = FR_NO_FILE;
    goto cleanup;
  }
  src_open = true;

  source_size_fs = f_size(&src_file);
  if (source_size_fs == 0 || source_size_fs > UINT32_MAX) {
    fr = FR_INT_ERR;
    goto cleanup;
  }
  source_size = (uint32_t)source_size_fs;
  if (source_size < (8u * NUM_BYTES_PER_SECTOR)) {
    fr = FR_INT_ERR;
    goto cleanup;
  }

  fr = readExact(&src_file, boot_sector, NUM_BYTES_PER_SECTOR);
  if (fr != FR_OK) goto cleanup;

  floppy_findDiskDetails(boot_sector, source_size, &sectors_per_track, &sides);
  if (sectors_per_track == 0 || sides == 0 || sides > 2) {
    fr = FR_INT_ERR;
    goto cleanup;
  }

  track_bytes = (uint32_t)sectors_per_track * NUM_BYTES_PER_SECTOR;
  if (track_bytes == 0 || track_bytes > ST_IMAGE_MAX_TRACK_BYTES ||
      (source_size % (track_bytes * (uint32_t)sides)) != 0) {
    fr = FR_INT_ERR;
    goto cleanup;
  }

  tracks = (uint16_t)(source_size / (track_bytes * (uint32_t)sides));
  if (tracks == 0 || tracks > 86) {
    fr = FR_INT_ERR;
    goto cleanup;
  }

  max_dest_size =
      source_size + sizeof(msa_header_t) + ((uint32_t)tracks * sides * 2u);
  fr = checkDiskSpace(folder, max_dest_size);
  if (fr != FR_OK) goto cleanup;

  track_buffer = malloc(track_bytes);
  encoded_track = malloc(track_bytes);
  if (!track_buffer || !encoded_track) {
    fr = FR_NOT_ENOUGH_CORE;
    goto cleanup;
  }

  fr = f_lseek(&src_file, 0);
  if (fr != FR_OK) goto cleanup;

  fr = f_open(&dest_file, dest_path, overwrite ? (FA_WRITE | FA_CREATE_ALWAYS)
                                               : (FA_WRITE | FA_CREATE_NEW));
  if (fr != FR_OK) {
    fr = (fr == FR_EXIST) ? FR_EXIST : FR_NO_FILE;
    goto cleanup;
  }
  dest_open = true;

  writeShortBE(header_buf, 0x0E0F);
  writeShortBE(header_buf + 2, sectors_per_track);
  writeShortBE(header_buf + 4, (uint16_t)(sides - 1));
  writeShortBE(header_buf + 6, 0);
  writeShortBE(header_buf + 8, (uint16_t)(tracks - 1));
  fr = writeExact(&dest_file, header_buf, sizeof(header_buf));
  if (fr != FR_OK) goto cleanup;

  for (uint16_t track = 0; track < tracks && fr == FR_OK; track++) {
    for (uint16_t side = 0; side < sides; side++) {
      size_t encoded_size = 0;

      fr = readExact(&src_file, track_buffer, (UINT)track_bytes);
      if (fr != FR_OK) goto cleanup;

      encoded_size = msa_measureEncodedTrack(track_buffer, (uint16_t)track_bytes);
      if (encoded_size < track_bytes) {
        uint16_t encoded_len = 0;
        if (!msa_encodeTrack(track_buffer, (uint16_t)track_bytes, encoded_track,
                             track_bytes, &encoded_len)) {
          fr = FR_INT_ERR;
          goto cleanup;
        }
        writeShortBE(length_buf, encoded_len);
        fr = writeExact(&dest_file, length_buf, sizeof(length_buf));
        if (fr != FR_OK) goto cleanup;
        fr = writeExact(&dest_file, encoded_track, encoded_len);
        if (fr != FR_OK) goto cleanup;
      } else {
        writeShortBE(length_buf, (uint16_t)track_bytes);
        fr = writeExact(&dest_file, length_buf, sizeof(length_buf));
        if (fr != FR_OK) goto cleanup;
        fr = writeExact(&dest_file, track_buffer, (UINT)track_bytes);
        if (fr != FR_OK) goto cleanup;
      }
    }
  }

cleanup:
  if (dest_open) {
    FRESULT close_fr = f_close(&dest_file);
    if (fr == FR_OK) fr = close_fr;
  }
  if (src_open) {
    FRESULT close_fr = f_close(&src_file);
    if (fr == FR_OK) fr = close_fr;
  }
  free(encoded_track);
  free(track_buffer);
  free(boot_sector);
  free(dest_path);
  free(src_path);
  if (fr != FR_OK) {
    DPRINTF("floppy_ST2MSA failed: src='%s' dst='%s' fr=%d\n",
            stFilename ? stFilename : "(null)",
            msaFilename ? msaFilename : "(null)", (int)fr);
  }
  return fr;
}
