/**
 * File: stx.c
 * Author: Diego Parrilla Santamaría
 * Date: July 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Pasti/STX reader -> standard logical sectors (FatFs-backed).
 *
 * Format (Pasti spec V0.5, Jean Louis-Guerin):
 *  File descriptor (16B): "RSY\0", u16 version(0x0300), u16 tool, u16 rsvd,
 *    u8 trackCount@0x0A, u8 revision@0x0B, u32 rsvd.
 *  Then trackCount Track records, each:
 *    Track descriptor (16B): u32 recordSize@0, u32 fuzzyCount@4,
 *      u16 sectorCount@8, u16 trackFlags@0x0A, u16 trackLength@0x0C,
 *      u8 trackNumber@0x0E (bit7=side, bits6-0=cyl), u8 trackType@0x0F.
 *    If trackFlags bit0 (TRK_SECT) SET: sectorCount * Sector descriptor (16B):
 *      u32 dataOffset@0, u16 bitPos@4, u16 readTime@6,
 *      id{u8 track,u8 head,u8 number,u8 size,u16 crc}@8, u8 fdcFlags@0x0E,
 *      u8 rsvd@0x0F. Then fuzzyCount fuzzy-mask bytes. Then Track Data:
 *      sector data at (trackDataPos + dataOffset), size 128<<id.size.
 *      trackDataPos = trackRecord + 16 + sectorCount*16 + fuzzyCount.
 *    If trackFlags bit0 CLEAR (standard track): 16B header immediately
 *      followed by sectorCount * 512 bytes, sectors 1..n in order.
 *    Next track record is at (current + recordSize).
 */

#include "include/stx.h"

#include <string.h>

#include "ff.h"
#include "include/debug.h"

#define STX_TRK_SECT 0x01u   // trackFlags bit0: has sector descriptors
#define STX_FDC_CLEAN 0x00u  // fdcFlags: standard sector when all bits clear
#define STX_SIZE_512 2u      // id.size code for 512-byte sectors

typedef struct {
  uint32_t record_offset;  // file offset of the track record
  uint32_t fuzzy_count;
  uint16_t sector_count;
  uint16_t track_flags;
  uint8_t track_number;  // raw: bit7=side
} stx_track_t;

static bool imageOpen = false;
static FIL stxFile;
static stx_track_t tracks[STX_MAX_TRACKS];
static int trackCount = 0;

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static bool read_at(uint32_t ofs, void *buf, uint32_t n) {
  if (f_lseek(&stxFile, (FSIZE_t)ofs) != FR_OK) return false;
  UINT br = 0;
  if (f_read(&stxFile, buf, (UINT)n, &br) != FR_OK) return false;
  return br == n;
}

stx_err_t stx_open(const char *path) {
  if (imageOpen) return STX_ERR_STATE;
  if (f_open(&stxFile, path, FA_READ) != FR_OK) {
    DPRINTF("stx: cannot open %s\n", path);
    return STX_ERR_OPEN;
  }

  uint8_t hdr[16];
  if (!read_at(0, hdr, 16) || memcmp(hdr, "RSY\0", 4) != 0) {
    DPRINTF("stx: bad magic\n");
    f_close(&stxFile);
    return STX_ERR_FORMAT;
  }
  int declaredTracks = hdr[0x0A];

  // Index the track records by walking recordSize.
  trackCount = 0;
  uint32_t ofs = 16;
  FSIZE_t size = f_size(&stxFile);
  for (int i = 0; i < declaredTracks && trackCount < STX_MAX_TRACKS; i++) {
    if ((FSIZE_t)(ofs + 16) > size) break;
    uint8_t td[16];
    if (!read_at(ofs, td, 16)) {
      f_close(&stxFile);
      return STX_ERR_READ;
    }
    uint32_t recordSize = rd_u32(td + 0);
    if (recordSize < 16 || (FSIZE_t)(ofs + recordSize) > size) break;
    stx_track_t *t = &tracks[trackCount++];
    t->record_offset = ofs;
    t->fuzzy_count = rd_u32(td + 4);
    t->sector_count = rd_u16(td + 8);
    t->track_flags = rd_u16(td + 0x0A);
    t->track_number = td[0x0E];
    ofs += recordSize;
  }
  if (trackCount == 0) {
    f_close(&stxFile);
    return STX_ERR_FORMAT;
  }
  imageOpen = true;
  DPRINTF("stx: opened, %d track records\n", trackCount);
  return STX_OK;
}

static const stx_track_t *find_track(uint8_t cyl, uint8_t side) {
  for (int i = 0; i < trackCount; i++) {
    const stx_track_t *t = &tracks[i];
    if ((t->track_number & 0x7Fu) == cyl && ((t->track_number >> 7) & 1u) == side) {
      return t;
    }
  }
  return NULL;
}

stx_err_t stx_get_geometry(stx_geometry_t *out) {
  if (!imageOpen || out == NULL) return STX_ERR_STATE;
  memset(out, 0, sizeof(*out));
  uint8_t maxCyl = 0;
  bool twoSided = false;
  for (int i = 0; i < trackCount; i++) {
    uint8_t cyl = tracks[i].track_number & 0x7Fu;
    if (cyl > maxCyl) maxCyl = cyl;
    if ((tracks[i].track_number >> 7) & 1u) twoSided = true;
  }
  out->sides = twoSided ? 2 : 1;
  out->cylinders = (uint8_t)(maxCyl + 1);
  // Sectors per track: use cyl0/side0, else the first indexed track.
  const stx_track_t *ref = find_track(0, 0);
  if (ref == NULL) ref = &tracks[0];
  out->sectors = (uint8_t)ref->sector_count;
  // Sanity: standard geometry has a uniform, sane sectors-per-track.
  out->standard_geometry =
      (out->sectors >= 8 && out->sectors <= 11 && out->cylinders >= 40 &&
       out->cylinders <= 84);
  return STX_OK;
}

stx_err_t stx_read_sector(uint8_t cyl, uint8_t side, uint8_t sector_number,
                          uint8_t *buf, stx_sector_result_t *result) {
  if (!imageOpen || buf == NULL) return STX_ERR_STATE;
  memset(buf, 0, STX_SECTOR_SIZE);
  if (result) *result = STX_SECTOR_MISSING;

  const stx_track_t *t = find_track(cyl, side);
  if (t == NULL || sector_number < 1 || sector_number > t->sector_count) {
    return STX_OK;  // MISSING, zero-filled
  }

  if (!(t->track_flags & STX_TRK_SECT)) {
    // Standard track: sectors 1..n, 512 bytes each, right after the header.
    uint32_t pos = t->record_offset + 16u + (uint32_t)(sector_number - 1) * 512u;
    if (!read_at(pos, buf, STX_SECTOR_SIZE)) return STX_ERR_READ;
    if (result) *result = STX_SECTOR_STANDARD;
    return STX_OK;
  }

  // Track with sector descriptors: find the matching clean-standard sector.
  uint32_t descBase = t->record_offset + 16u;
  uint32_t dataBase = descBase + (uint32_t)t->sector_count * 16u + t->fuzzy_count;
  for (int i = 0; i < t->sector_count; i++) {
    uint8_t sd[16];
    if (!read_at(descBase + (uint32_t)i * 16u, sd, 16)) return STX_ERR_READ;
    uint8_t idNumber = sd[0x0A];
    if (idNumber != sector_number) continue;
    uint8_t idSize = sd[0x0B];
    uint8_t fdcFlags = sd[0x0E];
    if (idSize != STX_SIZE_512 || fdcFlags != STX_FDC_CLEAN) {
      // Present but protected / wrong size / no data: non-standard.
      if (result) *result = STX_SECTOR_NONSTANDARD;
      return STX_OK;  // zero-filled
    }
    uint32_t dataOffset = rd_u32(sd + 0);
    if (!read_at(dataBase + dataOffset, buf, STX_SECTOR_SIZE)) return STX_ERR_READ;
    if (result) *result = STX_SECTOR_STANDARD;
    return STX_OK;
  }
  // Address exists in geometry but no descriptor matched: missing.
  return STX_OK;
}

void stx_close(void) {
  if (!imageOpen) return;
  f_close(&stxFile);
  imageOpen = false;
  trackCount = 0;
}

const char *stx_err_str(stx_err_t err) {
  switch (err) {
    case STX_OK:
      return "OK";
    case STX_ERR_OPEN:
      return "cannot open STX";
    case STX_ERR_FORMAT:
      return "not a valid Pasti/STX file";
    case STX_ERR_READ:
      return "read error";
    case STX_ERR_STATE:
      return "no STX open";
    default:
      return "unknown error";
  }
}

// --------------------------------------------------------------------------
// Background STX -> ST conversion job
// --------------------------------------------------------------------------

static stx_job_info_t job = {.status = STX_JOB_IDLE};
static FIL jobOut;

// Replace the source file's extension with ".st" in the destination folder.
static void build_dest_path(const char *folder, const char *stx_path,
                            char *out, size_t outSize) {
  const char *base = strrchr(stx_path, '/');
  base = base ? base + 1 : stx_path;
  char name[STX_PATH_MAX];
  snprintf(name, sizeof(name), "%s", base);
  char *dot = strrchr(name, '.');
  if (dot != NULL) *dot = '\0';
  const char *sep = (folder && folder[0] && folder[strlen(folder) - 1] == '/')
                        ? ""
                        : "/";
  snprintf(out, outSize, "%s%s%s.st", folder ? folder : "/", sep, name);
}

static void job_fail(stx_err_t err) {
  f_close(&jobOut);
  if (job.dest[0]) f_unlink(job.dest);
  stx_close();
  job.status = STX_JOB_FAILED;
  job.last_error = err;
}

bool stx_job_start(const char *stx_path, const char *dest_folder) {
  if (job.status == STX_JOB_CONVERTING) return false;
  memset(&job, 0, sizeof(job));
  snprintf(job.src, sizeof(job.src), "%s", stx_path ? stx_path : "");

  stx_err_t rc = stx_open(job.src);
  if (rc != STX_OK) {
    job.status = STX_JOB_FAILED;
    job.last_error = rc;
    return false;
  }
  if (stx_get_geometry(&job.geom) != STX_OK || job.geom.sectors == 0 ||
      job.geom.cylinders == 0 || job.geom.sides == 0) {
    stx_close();
    job.status = STX_JOB_FAILED;
    job.last_error = STX_ERR_FORMAT;
    return false;
  }

  build_dest_path(dest_folder, job.src, job.dest, sizeof(job.dest));
  f_unlink(job.dest);  // overwrite
  if (f_open(&jobOut, job.dest, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
    stx_close();
    job.dest[0] = '\0';
    job.status = STX_JOB_FAILED;
    job.last_error = STX_ERR_OPEN;
    return false;
  }

  job.tracks_total = (int)job.geom.cylinders * (int)job.geom.sides;
  job.tracks_done = 0;
  job.status = STX_JOB_CONVERTING;
  DPRINTF("stx: converting %s -> %s (%ux%ux%u)\n", job.src, job.dest,
          job.geom.cylinders, job.geom.sides, job.geom.sectors);
  return true;
}

void stx_job_poll(void) {
  if (job.status != STX_JOB_CONVERTING) return;
  if (job.cancel_requested) {
    f_close(&jobOut);
    if (job.dest[0]) f_unlink(job.dest);  // partial image is useless
    stx_close();
    job.status = STX_JOB_CANCELLED;
    return;
  }
  if (job.tracks_done >= job.tracks_total) {
    if (f_close(&jobOut) != FR_OK) {
      job_fail(STX_ERR_READ);
      return;
    }
    stx_close();
    job.status = STX_JOB_COMPLETED;
    return;
  }

  // .ST order is cylinder-major, head-minor.
  int idx = job.tracks_done;
  uint8_t cyl = (uint8_t)(idx / job.geom.sides);
  uint8_t side = (uint8_t)(idx % job.geom.sides);
  bool trackIncomplete = false;
  uint8_t buf[STX_SECTOR_SIZE];
  for (uint8_t s = 1; s <= job.geom.sectors; s++) {
    stx_sector_result_t r;
    if (stx_read_sector(cyl, side, s, buf, &r) != STX_OK) {
      job_fail(STX_ERR_READ);
      return;
    }
    UINT bw = 0;
    if (f_write(&jobOut, buf, STX_SECTOR_SIZE, &bw) != FR_OK ||
        bw != STX_SECTOR_SIZE) {
      job_fail(STX_ERR_READ);  // out of space / write error
      return;
    }
    if (r == STX_SECTOR_STANDARD) {
      job.sectors_standard++;
    } else if (r == STX_SECTOR_MISSING) {
      job.sectors_missing++;
      trackIncomplete = true;
    } else {
      job.sectors_nonstandard++;
      trackIncomplete = true;
    }
  }
  if (trackIncomplete) job.incomplete_tracks++;
  job.tracks_done++;
}

void stx_job_cancel(void) {
  if (job.status == STX_JOB_CONVERTING) job.cancel_requested = true;
}

bool stx_job_is_active(void) { return job.status == STX_JOB_CONVERTING; }

void stx_job_get_info(stx_job_info_t *out) {
  if (out != NULL) *out = job;
}

const char *stx_job_status_str(stx_job_status_t status) {
  switch (status) {
    case STX_JOB_IDLE:
      return "idle";
    case STX_JOB_CONVERTING:
      return "converting";
    case STX_JOB_COMPLETED:
      return "completed";
    case STX_JOB_FAILED:
      return "failed";
    case STX_JOB_CANCELLED:
      return "cancelled";
    default:
      return "unknown";
  }
}
