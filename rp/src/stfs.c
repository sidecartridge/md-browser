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
#define STFS_SFN_DIR_LEN 11u
#define STFS_SFN_SEQ_LIMIT 100u
#define STFS_DELETE_MAX_DEPTH 16u

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

static bool stfs_is_path_separator(unsigned char c) {
  return c == '/' || c == '\\';
}

static bool stfs_is_invalid_lfn_char(unsigned char c) {
  if (c < ' ' || c == 0x7Fu) return true;
  if (stfs_is_path_separator(c)) return true;
  return strchr("*:<>|\"?", (int)c) != NULL;
}

static bool stfs_is_invalid_sfn_char(unsigned char c) {
  return c == 0u || strchr("+,;=[]", (int)c) != NULL;
}

static unsigned char stfs_upper_ascii(unsigned char c) {
  return (c >= 'a' && c <= 'z') ? (unsigned char)(c - ('a' - 'A')) : c;
}

static void stfs_format_dir_name(const uint8_t *dir_name, char *out,
                                 size_t out_len) {
  size_t pos = 0;
  int base_end = 7;
  int ext_end = 10;

  if (!out || out_len == 0u) return;
  out[0] = '\0';
  if (!dir_name) return;

  while (base_end >= 0 && dir_name[base_end] == ' ') base_end--;
  while (ext_end >= 8 && dir_name[ext_end] == ' ') ext_end--;

  for (int i = 0; i <= base_end && pos + 1u < out_len; i++) {
    out[pos++] = (char)dir_name[i];
  }
  if (ext_end >= 8 && pos + 1u < out_len) {
    out[pos++] = '.';
    for (int i = 8; i <= ext_end && pos + 1u < out_len; i++) {
      out[pos++] = (char)dir_name[i];
    }
  }
  out[pos] = '\0';
}

static void stfs_generate_numbered_sfn(uint8_t *dst, const uint8_t *src,
                                       const char *source_name, uint32_t seq) {
  uint8_t suffix[8];
  size_t source_len = 0;
  uint32_t sreg = 0;
  unsigned i = 0;
  unsigned j = 0;

  memcpy(dst, src, STFS_SFN_DIR_LEN);

  if (seq > 5u) {
    sreg = seq;
    source_len = strlen(source_name);
    for (size_t idx = 0; idx < source_len; idx++) {
      uint16_t wc = (uint8_t)source_name[idx];
      for (i = 0; i < 16u; i++) {
        sreg = (sreg << 1u) + (uint32_t)(wc & 1u);
        wc >>= 1u;
        if ((sreg & 0x10000u) != 0u) sreg ^= 0x11021u;
      }
    }
    seq = sreg;
  }

  i = 7u;
  do {
    uint8_t c = (uint8_t)((seq % 16u) + '0');
    seq /= 16u;
    if (c > '9') c = (uint8_t)(c + 7u);
    suffix[i--] = c;
  } while (i != 0u && seq != 0u);
  suffix[i] = '~';

  while (j < i && dst[j] != ' ') j++;
  while (j < 8u) {
    dst[j++] = (i < 8u) ? suffix[i++] : ' ';
  }
}

static FRESULT stfs_build_base_sfn(const char *source_name, uint8_t *dir_name,
                                   bool *lossy) {
  const unsigned char *name = (const unsigned char *)source_name;
  size_t len = 0;
  size_t start = 0;
  size_t last_dot = SIZE_MAX;
  size_t body_end = 0;
  size_t body_len = 0;
  size_t ext_len = 0;
  bool local_lossy = false;

  if (!source_name || !dir_name) return FR_INVALID_PARAMETER;

  len = strlen(source_name);
  while (len > 0u && (name[len - 1u] == ' ' || name[len - 1u] == '.')) {
    len--;
    local_lossy = true;
  }
  while (start < len && (name[start] == ' ' || name[start] == '.')) {
    start++;
    local_lossy = true;
  }
  if (start >= len) return FR_INVALID_NAME;

  for (size_t idx = start; idx < len; idx++) {
    if (stfs_is_invalid_lfn_char(name[idx])) return FR_INVALID_NAME;
    if (name[idx] == '.') last_dot = idx;
  }

  body_end = (last_dot != SIZE_MAX) ? last_dot : len;
  memset(dir_name, ' ', STFS_SFN_DIR_LEN);

  for (size_t idx = start; idx < body_end; idx++) {
    unsigned char c = name[idx];

    if (c == ' ' || c == '.') {
      local_lossy = true;
      continue;
    }

    if (body_len >= 8u) {
      local_lossy = true;
      continue;
    }

    if (stfs_is_invalid_sfn_char(c)) {
      c = '_';
      local_lossy = true;
    } else if (c >= 'a' && c <= 'z') {
      c = stfs_upper_ascii(c);
    }

    dir_name[body_len++] = c;
  }

  if (body_len == 0u) return FR_INVALID_NAME;

  if (last_dot != SIZE_MAX && last_dot + 1u < len) {
    for (size_t idx = last_dot + 1u; idx < len; idx++) {
      unsigned char c = name[idx];

      if (c == ' ' || c == '.') {
        local_lossy = true;
        continue;
      }

      if (ext_len >= 3u) {
        local_lossy = true;
        continue;
      }

      if (stfs_is_invalid_sfn_char(c)) {
        c = '_';
        local_lossy = true;
      } else if (c >= 'a' && c <= 'z') {
        c = stfs_upper_ascii(c);
      }

      dir_name[8u + ext_len++] = c;
    }
  }

  if (lossy) *lossy = local_lossy;
  return FR_OK;
}

bool stfs_is_canonical_sfn_name(const char *name) {
  size_t len = 0;
  size_t dot = SIZE_MAX;
  size_t body_len = 0;
  size_t ext_len = 0;

  if (!name || name[0] == '\0') return false;

  len = strlen(name);
  if (name[0] == '.' || name[len - 1u] == '.') return false;

  for (size_t idx = 0; idx < len; idx++) {
    unsigned char c = (unsigned char)name[idx];

    if (c == '.') {
      if (dot != SIZE_MAX) return false;
      dot = idx;
      continue;
    }
    if (c == ' ' || stfs_is_path_separator(c) || stfs_is_invalid_lfn_char(c) ||
        stfs_is_invalid_sfn_char(c)) {
      return false;
    }
    if (c >= 'a' && c <= 'z') return false;
    if (dot == SIZE_MAX) {
      body_len++;
      if (body_len > 8u) return false;
    } else {
      ext_len++;
      if (ext_len > 3u) return false;
    }
  }

  return body_len > 0u && (dot == SIZE_MAX || ext_len > 0u);
}

bool stfs_can_browse_filename(const char *name) {
  size_t len = 0;

  if (!name) return false;

  len = strlen(name);
  return (len >= 3u && strcasecmp(name + len - 3u, ".st") == 0) ||
         (len >= 6u && strcasecmp(name + len - 6u, ".st.rw") == 0);
}

bool stfs_can_write_filename(const char *name) {
  size_t len = 0;

  if (!name) return false;

  len = strlen(name);
  return len >= 6u && strcasecmp(name + len - 6u, ".st.rw") == 0;
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

static FRESULT stfs_write_exact(stfs_t *fs, FSIZE_t offset, const void *buffer,
                                UINT bytes) {
  UINT written = 0;
  FRESULT fr = FR_OK;

  if (!fs || !fs->file_open || !fs->writable || !buffer) {
    return FR_INVALID_PARAMETER;
  }

  fr = f_lseek(&fs->image_file, offset);
  if (fr != FR_OK) return fr;

  fr = f_write(&fs->image_file, buffer, bytes, &written);
  if (fr != FR_OK) return fr;
  return written == bytes ? FR_OK : FR_DISK_ERR;
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

static FRESULT stfs_write_sector(stfs_t *fs, uint32_t sector) {
  if (!fs || !fs->sector_buffer) return FR_INVALID_PARAMETER;
  return stfs_write_exact(fs, (FSIZE_t)sector * fs->bytes_per_sector,
                          fs->sector_buffer, fs->bytes_per_sector);
}

static uint16_t stfs_eoc_value(const stfs_t *fs) {
  return (fs && fs->fat_type == STFS_FAT12) ? 0x0FFFu : 0xFFFFu;
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

static FRESULT stfs_flush_fat_bytes(stfs_t *fs, uint32_t fat_offset,
                                    uint32_t byte_count) {
  FRESULT fr = FR_OK;

  if (!fs || !fs->fat || !fs->writable) return FR_INVALID_PARAMETER;
  if (fat_offset + byte_count > fs->fat_size_bytes) return FR_INVALID_PARAMETER;

  for (uint8_t fat_index = 0u; fat_index < fs->fat_count; fat_index++) {
    FSIZE_t write_offset =
        ((FSIZE_t)(fs->fat_start_sector +
                   ((uint32_t)fat_index * fs->fat_size_sectors)) *
         fs->bytes_per_sector) +
        fat_offset;
    fr = stfs_write_exact(fs, write_offset, fs->fat + fat_offset,
                          (UINT)byte_count);
    if (fr != FR_OK) return fr;
  }

  return FR_OK;
}

static FRESULT stfs_set_fat_entry(stfs_t *fs, uint16_t cluster, uint16_t value) {
  uint32_t offset = 0;

  if (!fs || !fs->fat || !fs->writable || !stfs_is_valid_cluster(fs, cluster)) {
    return FR_INVALID_PARAMETER;
  }

  if (fs->fat_type == STFS_FAT12) {
    offset = ((uint32_t)cluster * 3u) / 2u;
    if (offset + 1u >= fs->fat_size_bytes) return FR_INT_ERR;

    if ((cluster & 1u) == 0u) {
      fs->fat[offset] = (uint8_t)(value & 0xFFu);
      fs->fat[offset + 1u] =
          (uint8_t)((fs->fat[offset + 1u] & 0xF0u) | ((value >> 8u) & 0x0Fu));
    } else {
      fs->fat[offset] =
          (uint8_t)((fs->fat[offset] & 0x0Fu) | ((value << 4u) & 0xF0u));
      fs->fat[offset + 1u] = (uint8_t)((value >> 4u) & 0xFFu);
    }
    return stfs_flush_fat_bytes(fs, offset, 2u);
  }

  offset = (uint32_t)cluster * 2u;
  if (offset + 1u >= fs->fat_size_bytes) return FR_INT_ERR;
  fs->fat[offset] = (uint8_t)(value & 0xFFu);
  fs->fat[offset + 1u] = (uint8_t)((value >> 8u) & 0xFFu);
  return stfs_flush_fat_bytes(fs, offset, 2u);
}

static FRESULT stfs_zero_cluster(stfs_t *fs, uint16_t cluster) {
  FRESULT fr = FR_OK;
  uint32_t sector = 0;

  if (!fs || !fs->sector_buffer || !stfs_is_valid_cluster(fs, cluster) ||
      !fs->writable) {
    return FR_INVALID_PARAMETER;
  }

  memset(fs->sector_buffer, 0, fs->bytes_per_sector);
  sector = stfs_cluster_to_sector(fs, cluster);
  for (uint8_t i = 0u; i < fs->sectors_per_cluster; i++) {
    fr = stfs_write_sector(fs, sector + i);
    if (fr != FR_OK) return fr;
  }

  return FR_OK;
}

static FRESULT stfs_allocate_chain(stfs_t *fs, uint32_t cluster_count,
                                   uint16_t *first_cluster) {
  uint16_t first = 0u;
  uint16_t prev = 0u;
  uint32_t allocated = 0u;
  FRESULT fr = FR_OK;
  uint32_t cluster_size = 0u;

  if (!fs || !first_cluster || !fs->writable) return FR_INVALID_PARAMETER;

  *first_cluster = 0u;
  if (cluster_count == 0u) return FR_OK;

  cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
  if (fs->free_bytes < (cluster_size * cluster_count)) return FR_DENIED;

  for (uint32_t cluster = 2u;
       cluster < fs->fat_entries && allocated < cluster_count; cluster++) {
    if (stfs_get_fat_entry(fs, (uint16_t)cluster) != 0u) continue;

    fr = stfs_set_fat_entry(fs, (uint16_t)cluster, stfs_eoc_value(fs));
    if (fr != FR_OK) goto rollback;

    if (prev != 0u) {
      fr = stfs_set_fat_entry(fs, prev, (uint16_t)cluster);
      if (fr != FR_OK) goto rollback;
    }

    if (first == 0u) first = (uint16_t)cluster;
    prev = (uint16_t)cluster;
    allocated++;
    if (fs->free_bytes >= cluster_size) fs->free_bytes -= cluster_size;
  }

  if (allocated != cluster_count) {
    fr = FR_DENIED;
    goto rollback;
  }

  *first_cluster = first;
  return FR_OK;

rollback:
  if (first != 0u) {
    uint16_t current = first;
    while (stfs_is_valid_cluster(fs, current)) {
      uint16_t next = stfs_get_fat_entry(fs, current);
      (void)stfs_set_fat_entry(fs, current, 0u);
      fs->free_bytes += cluster_size;
      if (!stfs_is_valid_cluster(fs, next) || stfs_is_eoc(fs, next)) break;
      current = next;
    }
  }
  *first_cluster = 0u;
  return fr;
}

static FRESULT stfs_free_chain(stfs_t *fs, uint16_t start_cluster) {
  uint16_t current = start_cluster;
  uint32_t cluster_size = 0u;

  if (!fs || !fs->writable) return FR_INVALID_PARAMETER;
  if (!stfs_is_valid_cluster(fs, start_cluster)) return FR_OK;

  cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
  while (stfs_is_valid_cluster(fs, current)) {
    uint16_t next = stfs_get_fat_entry(fs, current);
    FRESULT fr = stfs_set_fat_entry(fs, current, 0u);
    if (fr != FR_OK) return fr;
    fs->free_bytes += cluster_size;
    if (!stfs_is_valid_cluster(fs, next) || stfs_is_eoc(fs, next)) break;
    current = next;
  }

  return FR_OK;
}

static void stfs_copy_sfn_name(const uint8_t *entry, char *out,
                               size_t out_len) {
  stfs_format_dir_name(entry, out, out_len);
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

typedef struct {
  uint32_t sector;
  uint16_t offset;
  bool found;
} stfs_entry_location_t;

typedef struct {
  stfs_dir_t dir;
  size_t path_len;
} stfs_delete_frame_t;

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

static bool stfs_copy_string(char *dst, size_t dst_len, const char *src) {
  size_t src_len = 0;

  if (!dst || !src || dst_len == 0u) return false;
  src_len = strlen(src);
  if (src_len >= dst_len) return false;
  memcpy(dst, src, src_len + 1u);
  return true;
}

static bool stfs_append_name(char *path, size_t path_len, const char *name,
                             size_t *new_len) {
  size_t len = 0;
  int written = 0;

  if (!path || !name || !new_len) return false;

  len = strlen(path);
  written = snprintf(path + len, path_len - len, "%s%s",
                     (len > 0u && path[len - 1u] != '/') ? "/" : "", name);
  if (written < 0 || (size_t)written >= (path_len - len)) return false;

  *new_len = len + (size_t)written;
  return true;
}

static bool stfs_split_parent_child(const char *path, char *parent,
                                    size_t parent_len, char *name,
                                    size_t name_len) {
  const char *slash = NULL;
  size_t child_len = 0;
  size_t parent_size = 0;

  if (!path || !parent || !name || path[0] != '/') return false;
  if (strcmp(path, "/") == 0) return false;

  slash = strrchr(path, '/');
  if (!slash) return false;

  child_len = strlen(slash + 1u);
  if (child_len == 0u || child_len >= name_len) return false;

  parent_size = (size_t)(slash - path);
  if (parent_size == 0u) {
    if (!stfs_copy_string(parent, parent_len, "/")) return false;
  } else {
    if (parent_size >= parent_len) return false;
    memcpy(parent, path, parent_size);
    parent[parent_size] = '\0';
  }

  memcpy(name, slash + 1u, child_len + 1u);
  return true;
}

static FRESULT stfs_name_to_dir_name(const char *name, uint8_t *dir_name) {
  bool lossy = false;
  FRESULT fr = FR_OK;

  if (!name || !dir_name) return FR_INVALID_PARAMETER;
  if (!stfs_is_canonical_sfn_name(name)) return FR_INVALID_NAME;

  fr = stfs_build_base_sfn(name, dir_name, &lossy);
  if (fr != FR_OK) return fr;
  return lossy ? FR_INVALID_NAME : FR_OK;
}

static FRESULT stfs_find_entry_location_in_directory(
    stfs_t *fs, uint16_t start_cluster, bool is_root, const char *name,
    stfs_dirent_t *entry, stfs_entry_location_t *location) {
  uint8_t wanted[STFS_SFN_DIR_LEN];
  FRESULT fr = FR_OK;

  if (!fs || !name || !location) return FR_INVALID_PARAMETER;

  fr = stfs_name_to_dir_name(name, wanted);
  if (fr != FR_OK) return fr;

  location->sector = 0u;
  location->offset = 0u;
  location->found = false;

  if (is_root) {
    uint32_t total_entries =
        ((uint32_t)fs->root_dir_sectors * fs->bytes_per_sector) /
        STFS_DIR_ENTRY_SIZE;
    for (uint32_t entry_index = 0; entry_index < total_entries; entry_index++) {
      uint32_t sector = fs->root_dir_start_sector +
                        ((entry_index * STFS_DIR_ENTRY_SIZE) /
                         fs->bytes_per_sector);
      uint32_t offset =
          (entry_index * STFS_DIR_ENTRY_SIZE) % fs->bytes_per_sector;
      fr = stfs_read_sector(fs, sector);
      if (fr != FR_OK) return fr;
      if (fs->sector_buffer[offset] == 0x00u) return FR_NO_FILE;
      if (fs->sector_buffer[offset] == 0xE5u) continue;
      if (fs->sector_buffer[offset + 11u] == STFS_ATTR_LFN ||
          (fs->sector_buffer[offset + 11u] & STFS_ATTR_VOLUME)) {
        continue;
      }
      if (memcmp(fs->sector_buffer + offset, wanted, STFS_SFN_DIR_LEN) != 0) {
        continue;
      }
      if (entry) {
        memset(entry, 0, sizeof(*entry));
        stfs_copy_sfn_name(fs->sector_buffer + offset, entry->name,
                           sizeof(entry->name));
        entry->attr = fs->sector_buffer[offset + 11u];
        entry->time = stfs_read_le16(fs->sector_buffer + offset + 22u);
        entry->date = stfs_read_le16(fs->sector_buffer + offset + 24u);
        entry->first_cluster = stfs_read_le16(fs->sector_buffer + offset + 26u);
        entry->size = stfs_read_le32(fs->sector_buffer + offset + 28u);
      }
      location->sector = sector;
      location->offset = (uint16_t)offset;
      location->found = true;
      return FR_OK;
    }
    return FR_NO_FILE;
  }

  {
    uint16_t cluster = start_cluster;
    uint32_t safety = 0u;

    while (stfs_is_valid_cluster(fs, cluster) && !stfs_is_eoc(fs, cluster)) {
      uint32_t first_sector = stfs_cluster_to_sector(fs, cluster);
      for (uint8_t sector_index = 0u; sector_index < fs->sectors_per_cluster;
           sector_index++) {
        fr = stfs_read_sector(fs, first_sector + sector_index);
        if (fr != FR_OK) return fr;
        for (uint32_t offset = 0u; offset < fs->bytes_per_sector;
             offset += STFS_DIR_ENTRY_SIZE) {
          if (fs->sector_buffer[offset] == 0x00u) return FR_NO_FILE;
          if (fs->sector_buffer[offset] == 0xE5u) continue;
          if (fs->sector_buffer[offset + 11u] == STFS_ATTR_LFN ||
              (fs->sector_buffer[offset + 11u] & STFS_ATTR_VOLUME)) {
            continue;
          }
          if (memcmp(fs->sector_buffer + offset, wanted, STFS_SFN_DIR_LEN) !=
              0) {
            continue;
          }
          if (entry) {
            memset(entry, 0, sizeof(*entry));
            stfs_copy_sfn_name(fs->sector_buffer + offset, entry->name,
                               sizeof(entry->name));
            entry->attr = fs->sector_buffer[offset + 11u];
            entry->time = stfs_read_le16(fs->sector_buffer + offset + 22u);
            entry->date = stfs_read_le16(fs->sector_buffer + offset + 24u);
            entry->first_cluster =
                stfs_read_le16(fs->sector_buffer + offset + 26u);
            entry->size = stfs_read_le32(fs->sector_buffer + offset + 28u);
          }
          location->sector = first_sector + sector_index;
          location->offset = (uint16_t)offset;
          location->found = true;
          return FR_OK;
        }
      }

      cluster = stfs_get_fat_entry(fs, cluster);
      if (!stfs_is_valid_cluster(fs, cluster) || stfs_is_eoc(fs, cluster)) {
        break;
      }
      safety++;
      if (safety > fs->cluster_count + 2u) return FR_INT_ERR;
    }
  }

  return FR_NO_FILE;
}

static FRESULT stfs_find_free_entry_slot(stfs_t *fs, const stfs_node_t *dir_node,
                                         uint32_t *sector, uint16_t *offset) {
  FRESULT fr = FR_OK;

  if (!fs || !dir_node || !sector || !offset || !fs->writable) {
    return FR_INVALID_PARAMETER;
  }

  if (dir_node->is_root) {
    uint32_t total_entries =
        ((uint32_t)fs->root_dir_sectors * fs->bytes_per_sector) /
        STFS_DIR_ENTRY_SIZE;
    for (uint32_t entry_index = 0; entry_index < total_entries; entry_index++) {
      uint32_t entry_sector = fs->root_dir_start_sector +
                              ((entry_index * STFS_DIR_ENTRY_SIZE) /
                               fs->bytes_per_sector);
      uint32_t entry_offset =
          (entry_index * STFS_DIR_ENTRY_SIZE) % fs->bytes_per_sector;
      fr = stfs_read_sector(fs, entry_sector);
      if (fr != FR_OK) return fr;
      if (fs->sector_buffer[entry_offset] == 0x00u ||
          fs->sector_buffer[entry_offset] == 0xE5u) {
        *sector = entry_sector;
        *offset = (uint16_t)entry_offset;
        return FR_OK;
      }
    }
    return FR_DENIED;
  }

  {
    uint16_t cluster = dir_node->entry.first_cluster;
    uint16_t last_cluster = cluster;
    uint32_t safety = 0u;

    while (stfs_is_valid_cluster(fs, cluster) && !stfs_is_eoc(fs, cluster)) {
      uint32_t first_sector = stfs_cluster_to_sector(fs, cluster);
      last_cluster = cluster;
      for (uint8_t sector_index = 0u; sector_index < fs->sectors_per_cluster;
           sector_index++) {
        fr = stfs_read_sector(fs, first_sector + sector_index);
        if (fr != FR_OK) return fr;
        for (uint32_t entry_off = 0u; entry_off < fs->bytes_per_sector;
             entry_off += STFS_DIR_ENTRY_SIZE) {
          if (fs->sector_buffer[entry_off] == 0x00u ||
              fs->sector_buffer[entry_off] == 0xE5u) {
            *sector = first_sector + sector_index;
            *offset = (uint16_t)entry_off;
            return FR_OK;
          }
        }
      }

      {
        uint16_t next = stfs_get_fat_entry(fs, cluster);
        if (!stfs_is_valid_cluster(fs, next) || stfs_is_eoc(fs, next)) break;
        cluster = next;
      }

      safety++;
      if (safety > fs->cluster_count + 2u) return FR_INT_ERR;
    }

    {
      uint16_t new_cluster = 0u;
      fr = stfs_allocate_chain(fs, 1u, &new_cluster);
      if (fr != FR_OK) return fr;
      fr = stfs_set_fat_entry(fs, last_cluster, new_cluster);
      if (fr != FR_OK) {
        (void)stfs_free_chain(fs, new_cluster);
        return fr;
      }
      fr = stfs_zero_cluster(fs, new_cluster);
      if (fr != FR_OK) {
        (void)stfs_free_chain(fs, new_cluster);
        (void)stfs_set_fat_entry(fs, last_cluster, stfs_eoc_value(fs));
        return fr;
      }
      *sector = stfs_cluster_to_sector(fs, new_cluster);
      *offset = 0u;
      return FR_OK;
    }
  }
}

static void stfs_fill_dir_entry(uint8_t *entry, const uint8_t *dir_name,
                                BYTE attr, WORD date, WORD time,
                                uint16_t first_cluster, DWORD size) {
  if (!entry || !dir_name) return;
  memset(entry, 0, STFS_DIR_ENTRY_SIZE);
  memcpy(entry, dir_name, STFS_SFN_DIR_LEN);
  entry[11] = attr;
  entry[22] = (uint8_t)(time & 0xFFu);
  entry[23] = (uint8_t)((time >> 8u) & 0xFFu);
  entry[24] = (uint8_t)(date & 0xFFu);
  entry[25] = (uint8_t)((date >> 8u) & 0xFFu);
  entry[26] = (uint8_t)(first_cluster & 0xFFu);
  entry[27] = (uint8_t)((first_cluster >> 8u) & 0xFFu);
  entry[28] = (uint8_t)(size & 0xFFu);
  entry[29] = (uint8_t)((size >> 8u) & 0xFFu);
  entry[30] = (uint8_t)((size >> 16u) & 0xFFu);
  entry[31] = (uint8_t)((size >> 24u) & 0xFFu);
}

static FRESULT stfs_write_dir_entry(stfs_t *fs, uint32_t sector,
                                    uint16_t offset, const uint8_t *entry) {
  FRESULT fr = FR_OK;

  if (!fs || !entry || offset + STFS_DIR_ENTRY_SIZE > fs->bytes_per_sector ||
      !fs->writable) {
    return FR_INVALID_PARAMETER;
  }

  fr = stfs_read_sector(fs, sector);
  if (fr != FR_OK) return fr;
  memcpy(fs->sector_buffer + offset, entry, STFS_DIR_ENTRY_SIZE);
  return stfs_write_sector(fs, sector);
}

static FRESULT stfs_mark_dir_entry_deleted(stfs_t *fs, uint32_t sector,
                                           uint16_t offset) {
  FRESULT fr = FR_OK;

  if (!fs || offset + STFS_DIR_ENTRY_SIZE > fs->bytes_per_sector ||
      !fs->writable) {
    return FR_INVALID_PARAMETER;
  }

  fr = stfs_read_sector(fs, sector);
  if (fr != FR_OK) return fr;
  fs->sector_buffer[offset] = 0xE5u;
  memset(fs->sector_buffer + offset + 1u, 0, STFS_DIR_ENTRY_SIZE - 1u);
  return stfs_write_sector(fs, sector);
}

static FRESULT stfs_directory_is_empty(stfs_t *fs, const char *path) {
  stfs_dir_t dir = {0};
  stfs_dirent_t entry = {0};
  FRESULT fr = FR_OK;

  if (!fs || !path) return FR_INVALID_PARAMETER;

  fr = stfs_opendir(fs, path, &dir);
  if (fr != FR_OK) return fr;

  fr = stfs_readdir(&dir, &entry);
  stfs_closedir(&dir);
  if (fr != FR_OK) return fr;
  return entry.name[0] == '\0' ? FR_OK : FR_DENIED;
}

static FRESULT stfs_create_entry(stfs_t *fs, const char *dir_path,
                                 const char *name, BYTE attr, WORD date,
                                 WORD time, uint16_t first_cluster, DWORD size,
                                 uint32_t *entry_sector,
                                 uint16_t *entry_offset) {
  stfs_node_t dir_node = {0};
  uint8_t dir_name[STFS_SFN_DIR_LEN];
  uint8_t raw_entry[STFS_DIR_ENTRY_SIZE];
  stfs_entry_location_t existing = {0};
  uint32_t sector = 0u;
  uint16_t offset = 0u;
  FRESULT fr = FR_OK;

  if (!fs || !dir_path || !name || !fs->writable) return FR_INVALID_PARAMETER;

  fr = stfs_resolve_path(fs, dir_path, &dir_node);
  if (fr != FR_OK) return fr;
  if (!(dir_node.is_root || (dir_node.entry.attr & STFS_ATTR_DIRECTORY))) {
    return FR_NO_PATH;
  }

  fr = stfs_find_entry_location_in_directory(
      fs, dir_node.entry.first_cluster, dir_node.is_root, name, NULL, &existing);
  if (fr == FR_OK) return FR_EXIST;
  if (fr != FR_NO_FILE) return fr;

  fr = stfs_name_to_dir_name(name, dir_name);
  if (fr != FR_OK) return fr;

  fr = stfs_find_free_entry_slot(fs, &dir_node, &sector, &offset);
  if (fr != FR_OK) return fr;

  stfs_fill_dir_entry(raw_entry, dir_name, attr, date, time, first_cluster,
                      size);
  fr = stfs_write_dir_entry(fs, sector, offset, raw_entry);
  if (fr != FR_OK) return fr;

  if (entry_sector) *entry_sector = sector;
  if (entry_offset) *entry_offset = offset;
  return FR_OK;
}

static FRESULT stfs_name_exists_in_directory(stfs_t *fs, const char *dir_path,
                                             const char *name, bool *exists) {
  stfs_node_t dir_node = {0};
  FRESULT fr = FR_OK;

  if (!fs || !dir_path || !name || !exists) return FR_INVALID_PARAMETER;

  fr = stfs_resolve_path(fs, dir_path, &dir_node);
  if (fr != FR_OK) return fr;
  if (!(dir_node.is_root || (dir_node.entry.attr & STFS_ATTR_DIRECTORY))) {
    return FR_NO_PATH;
  }

  fr = stfs_find_in_directory(fs, dir_node.entry.first_cluster, dir_node.is_root,
                              name, NULL);
  if (fr == FR_OK) {
    *exists = true;
    return FR_OK;
  }
  if (fr == FR_NO_FILE) {
    *exists = false;
    return FR_OK;
  }
  return fr;
}

FRESULT stfs_resolve_sfn_name(stfs_t *fs, const char *dir_path,
                              const char *source_name, char *resolved_name,
                              size_t resolved_name_len, bool *renamed) {
  uint8_t base_dir_name[STFS_SFN_DIR_LEN];
  uint8_t candidate_dir_name[STFS_SFN_DIR_LEN];
  bool lossy = false;
  bool exists = false;
  bool local_renamed = false;
  FRESULT fr = FR_OK;

  if (!fs || !dir_path || !source_name || !resolved_name ||
      resolved_name_len == 0u) {
    return FR_INVALID_PARAMETER;
  }
  if (resolved_name_len < STFS_MAX_NAME_LEN) return FR_INVALID_PARAMETER;

  fr = stfs_build_base_sfn(source_name, base_dir_name, &lossy);
  if (fr != FR_OK) return fr;

  memcpy(candidate_dir_name, base_dir_name, sizeof(candidate_dir_name));
  stfs_format_dir_name(candidate_dir_name, resolved_name, resolved_name_len);
  local_renamed = !stfs_is_canonical_sfn_name(source_name);

  if (lossy) {
    for (uint32_t seq = 1u; seq < STFS_SFN_SEQ_LIMIT; seq++) {
      stfs_generate_numbered_sfn(candidate_dir_name, base_dir_name, source_name,
                                 seq);
      stfs_format_dir_name(candidate_dir_name, resolved_name, resolved_name_len);
      fr = stfs_name_exists_in_directory(fs, dir_path, resolved_name, &exists);
      if (fr != FR_OK) return fr;
      if (!exists) {
        if (renamed) *renamed = true;
        return FR_OK;
      }
    }
    return FR_DENIED;
  }

  fr = stfs_name_exists_in_directory(fs, dir_path, resolved_name, &exists);
  if (fr != FR_OK) return fr;
  if (exists) return FR_EXIST;

  if (renamed) *renamed = local_renamed;
  return FR_OK;
}

FRESULT stfs_rename(stfs_t *fs, const char *path, const char *new_name) {
  char parent_path[STFS_MAX_PATH_LEN];
  char old_name[STFS_MAX_NAME_LEN];
  uint8_t new_dir_name[STFS_SFN_DIR_LEN];
  stfs_node_t parent_node = {0};
  stfs_entry_location_t source_location = {0};
  stfs_entry_location_t existing_location = {0};
  FRESULT fr = FR_OK;

  if (!fs || !path || !new_name || !fs->writable) return FR_INVALID_PARAMETER;
  if (strcmp(path, "/") == 0) return FR_DENIED;
  if (!stfs_split_parent_child(path, parent_path, sizeof(parent_path), old_name,
                               sizeof(old_name))) {
    return FR_INVALID_NAME;
  }
  if (stfs_match_ci(old_name, new_name)) return FR_OK;

  fr = stfs_name_to_dir_name(new_name, new_dir_name);
  if (fr != FR_OK) return fr;

  fr = stfs_resolve_path(fs, parent_path, &parent_node);
  if (fr != FR_OK) return fr;
  if (!(parent_node.is_root || (parent_node.entry.attr & STFS_ATTR_DIRECTORY))) {
    return FR_NO_PATH;
  }

  fr = stfs_find_entry_location_in_directory(fs, parent_node.entry.first_cluster,
                                             parent_node.is_root, old_name,
                                             NULL, &source_location);
  if (fr != FR_OK) return fr;

  fr = stfs_find_entry_location_in_directory(fs, parent_node.entry.first_cluster,
                                             parent_node.is_root, new_name,
                                             NULL, &existing_location);
  if (fr == FR_OK) return FR_EXIST;
  if (fr != FR_NO_FILE) return fr;

  fr = stfs_read_sector(fs, source_location.sector);
  if (fr != FR_OK) return fr;
  memcpy(fs->sector_buffer + source_location.offset, new_dir_name,
         STFS_SFN_DIR_LEN);
  fr = stfs_write_sector(fs, source_location.sector);
  if (fr != FR_OK) return fr;

  return f_sync(&fs->image_file);
}

static FRESULT stfs_open_internal(stfs_t *fs, const char *image_path,
                                  bool writable) {
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

  fs->writable = writable;
  fr = f_open(&fs->image_file, image_path,
              writable ? (FA_READ | FA_WRITE) : FA_READ);
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

FRESULT stfs_open(stfs_t *fs, const char *image_path) {
  return stfs_open_internal(fs, image_path, false);
}

FRESULT stfs_open_rw(stfs_t *fs, const char *image_path) {
  return stfs_open_internal(fs, image_path, true);
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

static FRESULT stfs_delete_path_internal(stfs_t *fs, const char *path,
                                         bool require_empty_dir) {
  char parent_path[STFS_MAX_PATH_LEN];
  char name[STFS_MAX_NAME_LEN];
  stfs_node_t parent_node = {0};
  stfs_dirent_t entry = {0};
  stfs_entry_location_t location = {0};
  FRESULT fr = FR_OK;

  if (!fs || !path || !fs->writable) return FR_INVALID_PARAMETER;
  if (strcmp(path, "/") == 0) return FR_DENIED;
  if (!stfs_split_parent_child(path, parent_path, sizeof(parent_path), name,
                               sizeof(name))) {
    return FR_INVALID_NAME;
  }

  fr = stfs_resolve_path(fs, parent_path, &parent_node);
  if (fr != FR_OK) return fr;
  if (!(parent_node.is_root || (parent_node.entry.attr & STFS_ATTR_DIRECTORY))) {
    return FR_NO_PATH;
  }

  fr = stfs_find_entry_location_in_directory(fs, parent_node.entry.first_cluster,
                                             parent_node.is_root, name, &entry,
                                             &location);
  if (fr != FR_OK) return fr;

  if (require_empty_dir && (entry.attr & STFS_ATTR_DIRECTORY)) {
    fr = stfs_directory_is_empty(fs, path);
    if (fr != FR_OK) return fr;
  }

  fr = stfs_mark_dir_entry_deleted(fs, location.sector, location.offset);
  if (fr != FR_OK) return fr;

  if (stfs_is_valid_cluster(fs, entry.first_cluster)) {
    fr = stfs_free_chain(fs, entry.first_cluster);
    if (fr != FR_OK) return fr;
  }

  return f_sync(&fs->image_file);
}

static FRESULT stfs_init_directory_cluster(stfs_t *fs, uint16_t self_cluster,
                                           uint16_t parent_cluster, WORD date,
                                           WORD time) {
  uint8_t dot_name[STFS_SFN_DIR_LEN];
  uint8_t dotdot_name[STFS_SFN_DIR_LEN];
  uint8_t dot_entry[STFS_DIR_ENTRY_SIZE];
  uint8_t dotdot_entry[STFS_DIR_ENTRY_SIZE];
  uint32_t sector = 0u;
  FRESULT fr = FR_OK;

  if (!fs || !fs->writable || !stfs_is_valid_cluster(fs, self_cluster)) {
    return FR_INVALID_PARAMETER;
  }

  memset(dot_name, ' ', sizeof(dot_name));
  memset(dotdot_name, ' ', sizeof(dotdot_name));
  dot_name[0] = '.';
  dotdot_name[0] = '.';
  dotdot_name[1] = '.';
  stfs_fill_dir_entry(dot_entry, dot_name, STFS_ATTR_DIRECTORY, date, time,
                      self_cluster, 0u);
  stfs_fill_dir_entry(dotdot_entry, dotdot_name, STFS_ATTR_DIRECTORY, date,
                      time, parent_cluster, 0u);

  sector = stfs_cluster_to_sector(fs, self_cluster);
  fr = stfs_read_sector(fs, sector);
  if (fr != FR_OK) return fr;
  memcpy(fs->sector_buffer, dot_entry, STFS_DIR_ENTRY_SIZE);
  memcpy(fs->sector_buffer + STFS_DIR_ENTRY_SIZE, dotdot_entry,
         STFS_DIR_ENTRY_SIZE);
  fr = stfs_write_sector(fs, sector);
  if (fr != FR_OK) return fr;

  return f_sync(&fs->image_file);
}

FRESULT stfs_create_file(stfs_t *fs, const char *dir_path, const char *name,
                         DWORD file_size, BYTE attr, WORD date, WORD time,
                         stfs_write_file_t *file) {
  uint16_t first_cluster = 0u;
  uint32_t cluster_size = 0u;
  uint32_t cluster_count = 0u;
  uint32_t entry_sector = 0u;
  uint16_t entry_offset = 0u;
  FRESULT fr = FR_OK;

  if (!fs || !dir_path || !name || !file || !fs->writable) {
    return FR_INVALID_PARAMETER;
  }

  memset(file, 0, sizeof(*file));
  cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
  cluster_count = file_size == 0u ? 0u : ((file_size + cluster_size - 1u) / cluster_size);

  if (cluster_count > 0u) {
    fr = stfs_allocate_chain(fs, cluster_count, &first_cluster);
    if (fr != FR_OK) return fr;
  }

  fr = stfs_create_entry(fs, dir_path, name, (BYTE)(attr & ~STFS_ATTR_VOLUME),
                         date, time, first_cluster, file_size, &entry_sector,
                         &entry_offset);
  if (fr != FR_OK) {
    if (first_cluster != 0u) (void)stfs_free_chain(fs, first_cluster);
    return fr;
  }

  file->fs = fs;
  file->active = true;
  file->entry_sector = entry_sector;
  file->entry_offset = entry_offset;
  file->first_cluster = first_cluster;
  file->current_cluster = first_cluster;
  file->file_size = file_size;
  return FR_OK;
}

FRESULT stfs_write_file(stfs_write_file_t *file, const void *buffer,
                        UINT bytes_to_write, UINT *bytes_written) {
  const uint8_t *src = (const uint8_t *)buffer;
  uint32_t cluster_size = 0u;

  if (!file || !file->fs || !file->active || !buffer || !bytes_written) {
    return FR_INVALID_PARAMETER;
  }

  *bytes_written = 0u;
  if (bytes_to_write == 0u || file->file_pos >= file->file_size) return FR_OK;

  cluster_size = file->fs->bytes_per_sector * file->fs->sectors_per_cluster;

  while (*bytes_written < bytes_to_write && file->file_pos < file->file_size) {
    uint32_t remaining_in_file = file->file_size - file->file_pos;
    uint32_t remaining_in_request = bytes_to_write - *bytes_written;
    uint32_t sector_index = 0u;
    uint32_t sector_offset = 0u;
    uint32_t chunk = 0u;
    uint32_t sector = 0u;
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

    fr = stfs_write_exact(file->fs,
                          ((FSIZE_t)sector * file->fs->bytes_per_sector) +
                              sector_offset,
                          src + *bytes_written, (UINT)chunk);
    if (fr != FR_OK) return fr;

    *bytes_written += (UINT)chunk;
    file->file_pos += chunk;
    file->cluster_offset += chunk;

    if (file->cluster_offset == cluster_size && file->file_pos < file->file_size) {
      uint16_t next_cluster =
          stfs_get_fat_entry(file->fs, file->current_cluster);
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

FRESULT stfs_close_write_file(stfs_write_file_t *file, bool commit) {
  FRESULT fr = FR_OK;
  FRESULT cleanup_fr = FR_OK;

  if (!file) return FR_INVALID_PARAMETER;
  if (!file->active) return FR_OK;
  if (!file->fs || !file->fs->writable) {
    memset(file, 0, sizeof(*file));
    return FR_INVALID_PARAMETER;
  }

  if (!commit || file->file_pos != file->file_size) {
    cleanup_fr =
        stfs_mark_dir_entry_deleted(file->fs, file->entry_sector, file->entry_offset);
    if (cleanup_fr == FR_OK && stfs_is_valid_cluster(file->fs, file->first_cluster)) {
      cleanup_fr = stfs_free_chain(file->fs, file->first_cluster);
    }
    if (cleanup_fr == FR_OK) cleanup_fr = f_sync(&file->fs->image_file);
    fr = cleanup_fr;
  } else {
    fr = f_sync(&file->fs->image_file);
  }

  memset(file, 0, sizeof(*file));
  return fr;
}

FRESULT stfs_mkdir(stfs_t *fs, const char *dir_path, const char *name,
                   BYTE attr, WORD date, WORD time) {
  stfs_node_t dir_node = {0};
  uint16_t new_cluster = 0u;
  BYTE dir_attr = (BYTE)((attr & ~STFS_ATTR_VOLUME) | STFS_ATTR_DIRECTORY);
  FRESULT fr = FR_OK;

  if (!fs || !dir_path || !name || !fs->writable) return FR_INVALID_PARAMETER;

  fr = stfs_resolve_path(fs, dir_path, &dir_node);
  if (fr != FR_OK) return fr;
  if (!(dir_node.is_root || (dir_node.entry.attr & STFS_ATTR_DIRECTORY))) {
    return FR_NO_PATH;
  }

  fr = stfs_allocate_chain(fs, 1u, &new_cluster);
  if (fr != FR_OK) return fr;

  fr = stfs_zero_cluster(fs, new_cluster);
  if (fr != FR_OK) {
    (void)stfs_free_chain(fs, new_cluster);
    return fr;
  }

  fr = stfs_init_directory_cluster(
      fs, new_cluster, dir_node.is_root ? 0u : dir_node.entry.first_cluster, date,
      time);
  if (fr != FR_OK) {
    (void)stfs_free_chain(fs, new_cluster);
    return fr;
  }

  fr = stfs_create_entry(fs, dir_path, name, dir_attr, date, time, new_cluster,
                         0u, NULL, NULL);
  if (fr != FR_OK) {
    (void)stfs_free_chain(fs, new_cluster);
    return fr;
  }

  return f_sync(&fs->image_file);
}

FRESULT stfs_delete(stfs_t *fs, const char *path) {
  return stfs_delete_path_internal(fs, path, true);
}

FRESULT stfs_delete_tree(stfs_t *fs, const char *path) {
  stfs_dirent_t root_entry = {0};
  stfs_delete_frame_t *stack = NULL;
  char *work_path = NULL;
  uint8_t depth = 0u;
  FRESULT fr = FR_OK;

  if (!fs || !path || !fs->writable) return FR_INVALID_PARAMETER;
  if (strcmp(path, "/") == 0) return FR_DENIED;

  fr = stfs_stat(fs, path, &root_entry);
  if (fr != FR_OK) return fr;
  if (!(root_entry.attr & STFS_ATTR_DIRECTORY)) {
    return stfs_delete_path_internal(fs, path, false);
  }

  stack = calloc(STFS_DELETE_MAX_DEPTH, sizeof(*stack));
  work_path = calloc(1, STFS_MAX_PATH_LEN);
  if (!stack || !work_path) {
    fr = FR_NOT_ENOUGH_CORE;
    goto cleanup;
  }
  if (!stfs_copy_string(work_path, STFS_MAX_PATH_LEN, path)) {
    fr = FR_INVALID_NAME;
    goto cleanup;
  }

  fr = stfs_opendir(fs, work_path, &stack[0].dir);
  if (fr != FR_OK) goto cleanup;
  stack[0].path_len = strlen(work_path);
  depth = 1u;

  while (depth > 0u) {
    stfs_delete_frame_t *frame = &stack[depth - 1u];
    stfs_dirent_t entry = {0};

    fr = stfs_readdir(&frame->dir, &entry);
    if (fr != FR_OK) break;

    if (entry.name[0] == '\0') {
      stfs_closedir(&frame->dir);
      fr = stfs_delete_path_internal(fs, work_path, false);
      if (fr != FR_OK) break;
      depth--;
      if (depth > 0u) work_path[stack[depth - 1u].path_len] = '\0';
      continue;
    }

    {
      size_t child_len = 0u;
      if (!stfs_append_name(work_path, STFS_MAX_PATH_LEN, entry.name,
                            &child_len)) {
        fr = FR_INVALID_NAME;
        break;
      }

      if (entry.attr & STFS_ATTR_DIRECTORY) {
        if (depth >= STFS_DELETE_MAX_DEPTH) {
          fr = FR_DENIED;
          break;
        }
        fr = stfs_opendir(fs, work_path, &stack[depth].dir);
        if (fr != FR_OK) break;
        stack[depth].path_len = child_len;
        depth++;
      } else {
        fr = stfs_delete_path_internal(fs, work_path, false);
        work_path[frame->path_len] = '\0';
        if (fr != FR_OK) break;
      }
    }
  }

cleanup:
  while (depth > 0u && stack) {
    depth--;
    stfs_closedir(&stack[depth].dir);
  }
  free(work_path);
  free(stack);
  return fr;
}
