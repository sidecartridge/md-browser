/**
 * File: stfs.c
 * Description: Read-only FAT12/16 access to Atari ST floppy images.
 */

#include "include/stfs.h"

#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

#include "debug.h"

#ifndef STFS_FAT12
#define STFS_FAT12 12
#endif

#ifndef STFS_FAT16
#define STFS_FAT16 16
#endif

#define STFS_DIR_ENTRY_SIZE 32u
#define STFS_ATTR_DIRECTORY 0x10u
#define STFS_ATTR_LFN 0x0Fu
#define STFS_ATTR_VOLUME 0x08u

typedef struct {
  bool is_root;
  stfs_dirent_t entry;
} stfs_node_t;

static uint16_t stfs_get_fat_entry(const stfs_t *fs, uint16_t cluster);

static uint16_t stfs_read_le16(const uint8_t *src) {
  return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8u));
}

static uint32_t stfs_read_le32(const uint8_t *src) {
  return (uint32_t)src[0] | ((uint32_t)src[1] << 8u) |
         ((uint32_t)src[2] << 16u) | ((uint32_t)src[3] << 24u);
}

static bool stfs_is_valid_cluster(const stfs_t *fs, uint16_t cluster) {
  return fs && cluster >= 2u && (uint32_t)cluster < fs->fat_entries;
}

static bool stfs_is_eoc(const stfs_t *fs, uint16_t value) {
  if (!fs) return true;
  return fs->fat_type == STFS_FAT12 ? (value >= 0x0FF8u) : (value >= 0xFFF8u);
}

static bool stfs_match_ci(const char *lhs, const char *rhs) {
  if (!lhs || !rhs) return false;
  while (*lhs != '\0' && *rhs != '\0') {
    if (tolower((unsigned char)*lhs) != tolower((unsigned char)*rhs)) {
      return false;
    }
    lhs++;
    rhs++;
  }
  return *lhs == '\0' && *rhs == '\0';
}

bool stfs_can_browse_filename(const char *name) {
  size_t len = 0;

  if (!name) return false;

  len = strlen(name);
  return (len >= 3u && strcasecmp(name + len - 3u, ".st") == 0) ||
         (len >= 6u && strcasecmp(name + len - 6u, ".st.rw") == 0);
}

static FRESULT stfs_read_exact(stfs_t *fs, FSIZE_t offset, void *buffer,
                               UINT bytes) {
  UINT read = 0;
  FRESULT fr = FR_OK;

  if (!fs || !fs->file_open || !buffer) return FR_INVALID_PARAMETER;

  fr = f_lseek(&fs->image_file, offset);
  if (fr != FR_OK) return fr;

  fr = f_read(&fs->image_file, buffer, bytes, &read);
  if (fr != FR_OK) return fr;
  return read == bytes ? FR_OK : FR_DISK_ERR;
}

static uint32_t stfs_count_free_clusters(const stfs_t *fs) {
  uint32_t free_clusters = 0;

  if (!fs || !fs->fat) return 0;

  for (uint16_t cluster = 2u; cluster < fs->fat_entries; cluster++) {
    if (stfs_get_fat_entry(fs, cluster) == 0u) {
      free_clusters++;
    }
  }

  return free_clusters;
}

static FRESULT stfs_read_sector(stfs_t *fs, uint32_t sector) {
  if (!fs || !fs->sector_buffer) return FR_INVALID_PARAMETER;
  return stfs_read_exact(fs, (FSIZE_t)sector * fs->bytes_per_sector,
                         fs->sector_buffer, fs->bytes_per_sector);
}

static uint32_t stfs_cluster_to_sector(const stfs_t *fs, uint16_t cluster) {
  return fs->data_start_sector +
         ((uint32_t)(cluster - 2u) * fs->sectors_per_cluster);
}

static uint16_t stfs_get_fat_entry(const stfs_t *fs, uint16_t cluster) {
  if (!fs || !fs->fat || !stfs_is_valid_cluster(fs, cluster)) return 0xFFFFu;

  if (fs->fat_type == STFS_FAT12) {
    uint32_t offset = ((uint32_t)cluster * 3u) / 2u;
    uint16_t value = (uint16_t)fs->fat[offset] |
                     ((uint16_t)fs->fat[offset + 1u] << 8u);
    return (cluster & 1u) ? (value >> 4u) : (value & 0x0FFFu);
  }

  return stfs_read_le16(fs->fat + ((uint32_t)cluster * 2u));
}

static void stfs_copy_sfn_name(const uint8_t *entry, char *out,
                               size_t out_len) {
  size_t pos = 0;
  int base_end = 7;
  int ext_end = 10;

  if (!out || out_len == 0u) return;
  out[0] = '\0';
  if (!entry) return;

  while (base_end >= 0 && entry[base_end] == ' ') base_end--;
  while (ext_end >= 8 && entry[ext_end] == ' ') ext_end--;

  for (int i = 0; i <= base_end && pos + 1u < out_len; i++) {
    out[pos++] = (char)entry[i];
  }
  if (ext_end >= 8 && pos + 1u < out_len) {
    out[pos++] = '.';
    for (int i = 8; i <= ext_end && pos + 1u < out_len; i++) {
      out[pos++] = (char)entry[i];
    }
  }
  out[pos] = '\0';
}

static bool stfs_should_skip_entry(const uint8_t *entry, stfs_dirent_t *parsed) {
  if (!entry || !parsed) return true;
  if (entry[0] == 0xE5u) return true;
  if (entry[11] == STFS_ATTR_LFN) return true;
  if (entry[11] & STFS_ATTR_VOLUME) return true;

  memset(parsed, 0, sizeof(*parsed));
  stfs_copy_sfn_name(entry, parsed->name, sizeof(parsed->name));
  if (parsed->name[0] == '\0') return true;
  if (strcmp(parsed->name, ".") == 0 || strcmp(parsed->name, "..") == 0) {
    return true;
  }

  parsed->attr = entry[11];
  parsed->time = stfs_read_le16(entry + 22);
  parsed->date = stfs_read_le16(entry + 24);
  parsed->first_cluster = stfs_read_le16(entry + 26);
  parsed->size = stfs_read_le32(entry + 28);
  return false;
}

static FRESULT stfs_foreach_root_entry(stfs_t *fs, stfs_list_callback_t callback,
                                       void *user_data, bool *found_terminator) {
  uint32_t total_entries = 0;

  if (!fs || !callback) return FR_INVALID_PARAMETER;

  total_entries = ((uint32_t)fs->root_dir_sectors * fs->bytes_per_sector) /
                  STFS_DIR_ENTRY_SIZE;
  for (uint32_t entry_index = 0; entry_index < total_entries; entry_index++) {
    uint32_t sector = fs->root_dir_start_sector +
                      ((entry_index * STFS_DIR_ENTRY_SIZE) / fs->bytes_per_sector);
    uint32_t offset = (entry_index * STFS_DIR_ENTRY_SIZE) % fs->bytes_per_sector;
    stfs_dirent_t dirent;
    FRESULT fr = FR_OK;

    fr = stfs_read_sector(fs, sector);
    if (fr != FR_OK) return fr;

    if (fs->sector_buffer[offset] == 0x00u) {
      if (found_terminator) *found_terminator = true;
      return FR_OK;
    }
    if (stfs_should_skip_entry(fs->sector_buffer + offset, &dirent)) continue;
    if (!callback(&dirent, user_data)) return FR_OK;
  }

  return FR_OK;
}

static FRESULT stfs_foreach_cluster_dir(stfs_t *fs, uint16_t start_cluster,
                                        stfs_list_callback_t callback,
                                        void *user_data, bool *found_terminator) {
  uint16_t cluster = start_cluster;
  uint32_t safety = 0;

  if (!fs || !callback || !stfs_is_valid_cluster(fs, start_cluster)) {
    return FR_INVALID_PARAMETER;
  }

  while (stfs_is_valid_cluster(fs, cluster) && !stfs_is_eoc(fs, cluster)) {
    uint32_t first_sector = stfs_cluster_to_sector(fs, cluster);

    for (uint8_t sector_index = 0; sector_index < fs->sectors_per_cluster;
         sector_index++) {
      FRESULT fr = stfs_read_sector(fs, first_sector + sector_index);
      if (fr != FR_OK) return fr;

      for (uint32_t offset = 0; offset < fs->bytes_per_sector;
           offset += STFS_DIR_ENTRY_SIZE) {
        stfs_dirent_t dirent;

        if (fs->sector_buffer[offset] == 0x00u) {
          if (found_terminator) *found_terminator = true;
          return FR_OK;
        }
        if (stfs_should_skip_entry(fs->sector_buffer + offset, &dirent)) {
          continue;
        }
        if (!callback(&dirent, user_data)) return FR_OK;
      }
    }

    cluster = stfs_get_fat_entry(fs, cluster);
    safety++;
    if (safety > fs->cluster_count + 2u) return FR_INT_ERR;
  }

  return FR_OK;
}

typedef struct {
  const char *name;
  stfs_dirent_t *entry;
  bool found;
} stfs_find_ctx_t;

static bool stfs_find_callback(const stfs_dirent_t *entry, void *user_data) {
  stfs_find_ctx_t *ctx = (stfs_find_ctx_t *)user_data;
  if (!ctx || !entry) return false;
  if (!stfs_match_ci(entry->name, ctx->name)) return true;
  if (ctx->entry) memcpy(ctx->entry, entry, sizeof(*ctx->entry));
  ctx->found = true;
  return false;
}

static FRESULT stfs_find_in_directory(stfs_t *fs, uint16_t start_cluster,
                                      bool is_root, const char *name,
                                      stfs_dirent_t *entry) {
  stfs_find_ctx_t ctx = {.name = name, .entry = entry, .found = false};
  FRESULT fr = FR_OK;

  if (is_root) {
    fr = stfs_foreach_root_entry(fs, stfs_find_callback, &ctx, NULL);
  } else {
    fr = stfs_foreach_cluster_dir(fs, start_cluster, stfs_find_callback, &ctx,
                                  NULL);
  }
  if (fr != FR_OK) return fr;
  return ctx.found ? FR_OK : FR_NO_FILE;
}

static FRESULT stfs_resolve_path(stfs_t *fs, const char *path,
                                 stfs_node_t *node) {
  char segment[STFS_MAX_NAME_LEN];
  const char *cursor = path;
  stfs_node_t current = {0};

  if (!fs || !path || !node) return FR_INVALID_PARAMETER;

  current.is_root = true;
  current.entry.attr = STFS_ATTR_DIRECTORY;
  current.entry.first_cluster = 0u;
  current.entry.name[0] = '/';
  current.entry.name[1] = '\0';

  while (*cursor == '/') cursor++;
  if (*cursor == '\0') {
    memcpy(node, &current, sizeof(*node));
    return FR_OK;
  }

  while (*cursor != '\0') {
    size_t len = 0;
    while (cursor[len] != '\0' && cursor[len] != '/') len++;
    if (len == 0u || len >= sizeof(segment)) return FR_INVALID_NAME;

    memcpy(segment, cursor, len);
    segment[len] = '\0';

    if (!(current.is_root || (current.entry.attr & STFS_ATTR_DIRECTORY))) {
      return FR_NO_PATH;
    }

    FRESULT fr = stfs_find_in_directory(fs, current.entry.first_cluster,
                                        current.is_root, segment,
                                        &current.entry);
    if (fr != FR_OK) return fr;
    current.is_root = false;

    cursor += len;
    while (*cursor == '/') cursor++;
  }

  memcpy(node, &current, sizeof(*node));
  return FR_OK;
}

FRESULT stfs_open(stfs_t *fs, const char *image_path) {
  uint8_t *boot_sector = NULL;
  uint32_t root_dir_bytes = 0;
  uint32_t data_sectors = 0;
  uint32_t min_bytes = 0;
  FSIZE_t file_size = 0;
  FRESULT fr = FR_OK;

  if (!fs || !image_path || image_path[0] == '\0') {
    return FR_INVALID_PARAMETER;
  }

  memset(fs, 0, sizeof(*fs));

  if (strlen(image_path) >= sizeof(fs->image_path)) return FR_INVALID_NAME;
  memcpy(fs->image_path, image_path, strlen(image_path) + 1u);

  fs->sector_buffer = calloc(1, 512u);
  if (!fs->sector_buffer) {
    fr = FR_NOT_ENOUGH_CORE;
    goto cleanup;
  }

  fr = f_open(&fs->image_file, image_path, FA_READ);
  if (fr != FR_OK) goto cleanup;
  fs->file_open = true;
  boot_sector = fs->sector_buffer;

  file_size = f_size(&fs->image_file);
  if (file_size < 512u) {
    fr = FR_NO_FILESYSTEM;
    goto cleanup;
  }

  fr = stfs_read_exact(fs, 0u, boot_sector, 512u);
  if (fr != FR_OK) goto cleanup;

  fs->bytes_per_sector = stfs_read_le16(boot_sector + 11);
  fs->sectors_per_cluster = boot_sector[13];
  fs->reserved_sectors = stfs_read_le16(boot_sector + 14);
  fs->fat_count = boot_sector[16];
  fs->root_entry_count = stfs_read_le16(boot_sector + 17);
  fs->total_sectors = stfs_read_le16(boot_sector + 19);
  fs->sectors_per_track = stfs_read_le16(boot_sector + 24);
  fs->head_count = stfs_read_le16(boot_sector + 26);
  if (fs->total_sectors == 0u) {
    fs->total_sectors = stfs_read_le32(boot_sector + 32);
  }
  fs->fat_size_sectors = stfs_read_le16(boot_sector + 22);

  if (fs->bytes_per_sector != 512u || fs->sectors_per_cluster == 0u ||
      (fs->sectors_per_cluster & (fs->sectors_per_cluster - 1u)) != 0u ||
      fs->reserved_sectors == 0u || fs->fat_count == 0u ||
      fs->root_entry_count == 0u || fs->total_sectors == 0u ||
      fs->fat_size_sectors == 0u) {
    fr = FR_NO_FILESYSTEM;
    goto cleanup;
  }

  root_dir_bytes = (uint32_t)fs->root_entry_count * STFS_DIR_ENTRY_SIZE;
  fs->root_dir_sectors =
      (root_dir_bytes + fs->bytes_per_sector - 1u) / fs->bytes_per_sector;
  fs->fat_start_sector = fs->reserved_sectors;
  fs->root_dir_start_sector =
      fs->fat_start_sector + ((uint32_t)fs->fat_count * fs->fat_size_sectors);
  fs->data_start_sector = fs->root_dir_start_sector + fs->root_dir_sectors;

  if (fs->total_sectors <= fs->data_start_sector) {
    fr = FR_NO_FILESYSTEM;
    goto cleanup;
  }

  data_sectors = fs->total_sectors - fs->data_start_sector;
  fs->cluster_count = data_sectors / fs->sectors_per_cluster;
  fs->fat_entries = fs->cluster_count + 2u;
  if (fs->cluster_count < 1u) {
    fr = FR_NO_FILESYSTEM;
    goto cleanup;
  }

  if (fs->cluster_count < 4085u) {
    fs->fat_type = STFS_FAT12;
  } else if (fs->cluster_count < 65525u) {
    fs->fat_type = STFS_FAT16;
  } else {
    fr = FR_NO_FILESYSTEM;
    goto cleanup;
  }

  min_bytes = fs->total_sectors * fs->bytes_per_sector;
  if (file_size < min_bytes) {
    fr = FR_NO_FILESYSTEM;
    goto cleanup;
  }

  fs->fat_size_bytes = fs->fat_size_sectors * fs->bytes_per_sector;
  fs->fat = malloc(fs->fat_size_bytes);
  if (!fs->fat) {
    fr = FR_NOT_ENOUGH_CORE;
    goto cleanup;
  }

  fr = stfs_read_exact(fs, (FSIZE_t)fs->fat_start_sector * fs->bytes_per_sector,
                       fs->fat, fs->fat_size_bytes);
  if (fr != FR_OK) goto cleanup;

  fs->total_bytes = fs->total_sectors * fs->bytes_per_sector;
  fs->free_bytes = stfs_count_free_clusters(fs) * fs->sectors_per_cluster *
                   fs->bytes_per_sector;
  if (fs->head_count > 0u && fs->sectors_per_track > 0u) {
    uint32_t sectors_per_trackset =
        (uint32_t)fs->head_count * fs->sectors_per_track;
    if (sectors_per_trackset > 0u &&
        (fs->total_sectors % sectors_per_trackset) == 0u) {
      fs->track_count = (uint16_t)(fs->total_sectors / sectors_per_trackset);
    }
  }

  return FR_OK;

cleanup:
  stfs_close(fs);
  return fr;
}

void stfs_close(stfs_t *fs) {
  if (!fs) return;

  if (fs->file_open) {
    (void)f_close(&fs->image_file);
  }
  fs->file_open = false;
  free(fs->fat);
  fs->fat = NULL;
  free(fs->sector_buffer);
  fs->sector_buffer = NULL;
  memset(fs, 0, sizeof(*fs));
}

void stfs_get_info(const stfs_t *fs, stfs_info_t *info) {
  if (!info) return;

  memset(info, 0, sizeof(*info));
  if (!fs) return;

  info->fat_type = fs->fat_type;
  info->sectors_per_track = fs->sectors_per_track;
  info->head_count = fs->head_count;
  info->track_count = fs->track_count;
  info->total_bytes = fs->total_bytes;
  info->free_bytes = fs->free_bytes;
}

FRESULT stfs_opendir(stfs_t *fs, const char *path, stfs_dir_t *dir) {
  stfs_node_t node = {0};
  FRESULT fr = FR_OK;

  if (!fs || !path || !dir) return FR_INVALID_PARAMETER;

  fr = stfs_resolve_path(fs, path, &node);
  if (fr != FR_OK) return fr;
  if (!node.is_root && !(node.entry.attr & STFS_ATTR_DIRECTORY)) {
    return FR_NO_PATH;
  }

  memset(dir, 0, sizeof(*dir));
  dir->fs = fs;
  dir->is_root = node.is_root;
  if (!node.is_root) {
    if (!stfs_is_valid_cluster(fs, node.entry.first_cluster)) return FR_INT_ERR;
    dir->current_cluster = node.entry.first_cluster;
  }

  return FR_OK;
}

FRESULT stfs_readdir(stfs_dir_t *dir, stfs_dirent_t *entry) {
  uint32_t total_entries = 0;

  if (!dir || !dir->fs || !entry) return FR_INVALID_PARAMETER;

  memset(entry, 0, sizeof(*entry));
  if (dir->end_reached) return FR_OK;

  if (dir->is_root) {
    total_entries = ((uint32_t)dir->fs->root_dir_sectors *
                     dir->fs->bytes_per_sector) /
                    STFS_DIR_ENTRY_SIZE;
    while (dir->root_entry_index < total_entries) {
      uint32_t entry_index = dir->root_entry_index++;
      uint32_t sector =
          dir->fs->root_dir_start_sector +
          ((entry_index * STFS_DIR_ENTRY_SIZE) / dir->fs->bytes_per_sector);
      uint32_t offset =
          (entry_index * STFS_DIR_ENTRY_SIZE) % dir->fs->bytes_per_sector;
      FRESULT fr = stfs_read_sector(dir->fs, sector);
      if (fr != FR_OK) return fr;

      if (dir->fs->sector_buffer[offset] == 0x00u) {
        dir->end_reached = true;
        return FR_OK;
      }
      if (stfs_should_skip_entry(dir->fs->sector_buffer + offset, entry)) {
        continue;
      }
      return FR_OK;
    }

    dir->end_reached = true;
    return FR_OK;
  }

  while (stfs_is_valid_cluster(dir->fs, dir->current_cluster) &&
         !stfs_is_eoc(dir->fs, dir->current_cluster)) {
    while (dir->sector_index < dir->fs->sectors_per_cluster) {
      uint32_t sector =
          stfs_cluster_to_sector(dir->fs, dir->current_cluster) +
          dir->sector_index;
      FRESULT fr = stfs_read_sector(dir->fs, sector);
      if (fr != FR_OK) return fr;

      while (dir->entry_offset < dir->fs->bytes_per_sector) {
        uint16_t offset = dir->entry_offset;
        dir->entry_offset =
            (uint16_t)(dir->entry_offset + STFS_DIR_ENTRY_SIZE);

        if (dir->fs->sector_buffer[offset] == 0x00u) {
          dir->end_reached = true;
          return FR_OK;
        }
        if (stfs_should_skip_entry(dir->fs->sector_buffer + offset, entry)) {
          continue;
        }
        return FR_OK;
      }

      dir->entry_offset = 0u;
      dir->sector_index++;
    }

    {
      uint16_t next_cluster = stfs_get_fat_entry(dir->fs, dir->current_cluster);
      if (!stfs_is_valid_cluster(dir->fs, next_cluster) ||
          stfs_is_eoc(dir->fs, next_cluster)) {
        dir->end_reached = true;
        return FR_OK;
      }

      dir->current_cluster = next_cluster;
      dir->sector_index = 0u;
      dir->entry_offset = 0u;
      dir->chain_safety++;
      if (dir->chain_safety > dir->fs->cluster_count + 2u) {
        return FR_INT_ERR;
      }
    }
  }

  dir->end_reached = true;
  return FR_OK;
}

void stfs_closedir(stfs_dir_t *dir) {
  if (!dir) return;
  memset(dir, 0, sizeof(*dir));
}

FRESULT stfs_open_file(stfs_t *fs, const char *path, stfs_file_t *file) {
  stfs_node_t node = {0};
  FRESULT fr = FR_OK;

  if (!fs || !path || !file) return FR_INVALID_PARAMETER;

  fr = stfs_resolve_path(fs, path, &node);
  if (fr != FR_OK) return fr;
  if (node.is_root || (node.entry.attr & STFS_ATTR_DIRECTORY)) {
    return FR_DENIED;
  }
  if (node.entry.size > 0u && !stfs_is_valid_cluster(fs, node.entry.first_cluster)) {
    return FR_INT_ERR;
  }

  memset(file, 0, sizeof(*file));
  file->fs = fs;
  file->start_cluster = node.entry.first_cluster;
  file->current_cluster = node.entry.first_cluster;
  file->file_size = node.entry.size;
  return FR_OK;
}

FRESULT stfs_read_file(stfs_file_t *file, void *buffer, UINT bytes_to_read,
                       UINT *bytes_read) {
  uint8_t *dst = (uint8_t *)buffer;
  uint32_t cluster_size = 0;

  if (!file || !file->fs || !buffer || !bytes_read) return FR_INVALID_PARAMETER;

  *bytes_read = 0;
  if (bytes_to_read == 0u || file->file_pos >= file->file_size) return FR_OK;

  cluster_size = file->fs->bytes_per_sector * file->fs->sectors_per_cluster;

  while (*bytes_read < bytes_to_read && file->file_pos < file->file_size) {
    uint32_t remaining_in_file = file->file_size - file->file_pos;
    uint32_t remaining_in_request = bytes_to_read - *bytes_read;
    uint32_t sector_index = 0;
    uint32_t sector_offset = 0;
    uint32_t chunk = 0;
    uint32_t sector = 0;
    FRESULT fr = FR_OK;

    if (!stfs_is_valid_cluster(file->fs, file->current_cluster)) {
      return FR_INT_ERR;
    }

    sector_index = file->cluster_offset / file->fs->bytes_per_sector;
    sector_offset = file->cluster_offset % file->fs->bytes_per_sector;
    sector = stfs_cluster_to_sector(file->fs, file->current_cluster) +
             sector_index;
    chunk = file->fs->bytes_per_sector - sector_offset;
    if (chunk > remaining_in_file) chunk = remaining_in_file;
    if (chunk > remaining_in_request) chunk = remaining_in_request;

    fr = stfs_read_exact(file->fs,
                         ((FSIZE_t)sector * file->fs->bytes_per_sector) +
                             sector_offset,
                         dst + *bytes_read, (UINT)chunk);
    if (fr != FR_OK) return fr;

    *bytes_read += (UINT)chunk;
    file->file_pos += chunk;
    file->cluster_offset += chunk;

    if (file->cluster_offset == cluster_size && file->file_pos < file->file_size) {
      uint16_t next_cluster = stfs_get_fat_entry(file->fs, file->current_cluster);
      if (!stfs_is_valid_cluster(file->fs, next_cluster) ||
          stfs_is_eoc(file->fs, next_cluster)) {
        return FR_INT_ERR;
      }

      file->current_cluster = next_cluster;
      file->cluster_offset = 0u;
      file->chain_safety++;
      if (file->chain_safety > file->fs->cluster_count + 2u) {
        return FR_INT_ERR;
      }
    }
  }

  return FR_OK;
}

void stfs_close_file(stfs_file_t *file) {
  if (!file) return;
  memset(file, 0, sizeof(*file));
}

FRESULT stfs_stat(stfs_t *fs, const char *path, stfs_dirent_t *entry) {
  stfs_node_t node = {0};
  FRESULT fr = FR_OK;

  if (!fs || !path || !entry) return FR_INVALID_PARAMETER;

  fr = stfs_resolve_path(fs, path, &node);
  if (fr != FR_OK) return fr;

  memset(entry, 0, sizeof(*entry));
  memcpy(entry, &node.entry, sizeof(*entry));
  if (node.is_root) {
    entry->attr = STFS_ATTR_DIRECTORY;
    entry->name[0] = '/';
    entry->name[1] = '\0';
  }
  return FR_OK;
}

FRESULT stfs_list_dir(stfs_t *fs, const char *path, stfs_list_callback_t callback,
                      void *user_data) {
  stfs_dir_t dir = {0};
  stfs_dirent_t entry = {0};
  FRESULT fr = FR_OK;

  if (!fs || !path || !callback) return FR_INVALID_PARAMETER;

  fr = stfs_opendir(fs, path, &dir);
  if (fr != FR_OK) return fr;

  while ((fr = stfs_readdir(&dir, &entry)) == FR_OK && entry.name[0] != '\0') {
    if (!callback(&entry, user_data)) break;
  }

  stfs_closedir(&dir);
  return fr;
}
