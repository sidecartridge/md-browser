/**
 * File: stfs.h
 * Description: FAT12/16 access to Atari ST floppy images.
 */

#ifndef STFS_H
#define STFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff.h"

#ifndef STFS_MAX_PATH_LEN
#define STFS_MAX_PATH_LEN 512
#endif

#ifndef STFS_MAX_NAME_LEN
#define STFS_MAX_NAME_LEN 13
#endif

typedef struct {
  char name[STFS_MAX_NAME_LEN];
  BYTE attr;
  DWORD size;
  WORD date;
  WORD time;
  uint16_t first_cluster;
} stfs_dirent_t;

typedef struct {
  FIL image_file;
  bool file_open;
  bool writable;
  char image_path[STFS_MAX_PATH_LEN];
  uint16_t bytes_per_sector;
  uint8_t sectors_per_cluster;
  uint16_t reserved_sectors;
  uint8_t fat_count;
  uint16_t root_entry_count;
  uint32_t total_sectors;
  uint32_t fat_size_sectors;
  uint32_t root_dir_sectors;
  uint32_t fat_start_sector;
  uint32_t root_dir_start_sector;
  uint32_t data_start_sector;
  uint32_t cluster_count;
  uint32_t fat_entries;
  uint8_t fat_type;
  uint16_t sectors_per_track;
  uint16_t head_count;
  uint16_t track_count;
  uint32_t total_bytes;
  uint32_t free_bytes;
  uint8_t *fat;
  uint32_t fat_size_bytes;
  uint8_t *sector_buffer;
} stfs_t;

typedef struct {
  stfs_t *fs;
  bool is_root;
  bool end_reached;
  uint32_t root_entry_index;
  uint16_t current_cluster;
  uint8_t sector_index;
  uint16_t entry_offset;
  uint32_t chain_safety;
} stfs_dir_t;

typedef struct {
  stfs_t *fs;
  uint16_t start_cluster;
  uint16_t current_cluster;
  uint32_t file_size;
  uint32_t file_pos;
  uint32_t cluster_offset;
  uint32_t chain_safety;
} stfs_file_t;

typedef struct {
  stfs_t *fs;
  bool active;
  uint32_t entry_sector;
  uint16_t entry_offset;
  uint16_t first_cluster;
  uint16_t current_cluster;
  uint32_t file_size;
  uint32_t file_pos;
  uint32_t cluster_offset;
  uint32_t chain_safety;
} stfs_write_file_t;

typedef struct {
  uint8_t fat_type;
  uint16_t sectors_per_track;
  uint16_t head_count;
  uint16_t track_count;
  uint32_t total_bytes;
  uint32_t free_bytes;
} stfs_info_t;

typedef bool (*stfs_list_callback_t)(const stfs_dirent_t *entry, void *user_data);

bool stfs_can_browse_filename(const char *name);
bool stfs_can_write_filename(const char *name);
bool stfs_is_canonical_sfn_name(const char *name);
FRESULT stfs_open(stfs_t *fs, const char *image_path);
FRESULT stfs_open_rw(stfs_t *fs, const char *image_path);
void stfs_close(stfs_t *fs);
void stfs_get_info(const stfs_t *fs, stfs_info_t *info);
FRESULT stfs_opendir(stfs_t *fs, const char *path, stfs_dir_t *dir);
FRESULT stfs_readdir(stfs_dir_t *dir, stfs_dirent_t *entry);
void stfs_closedir(stfs_dir_t *dir);
FRESULT stfs_open_file(stfs_t *fs, const char *path, stfs_file_t *file);
FRESULT stfs_read_file(stfs_file_t *file, void *buffer, UINT bytes_to_read,
                       UINT *bytes_read);
void stfs_close_file(stfs_file_t *file);
FRESULT stfs_stat(stfs_t *fs, const char *path, stfs_dirent_t *entry);
FRESULT stfs_list_dir(stfs_t *fs, const char *path, stfs_list_callback_t callback,
                      void *user_data);
FRESULT stfs_resolve_sfn_name(stfs_t *fs, const char *dir_path,
                              const char *source_name, char *resolved_name,
                              size_t resolved_name_len, bool *renamed);
FRESULT stfs_rename(stfs_t *fs, const char *path, const char *new_name);
FRESULT stfs_create_file(stfs_t *fs, const char *dir_path, const char *name,
                         DWORD file_size, BYTE attr, WORD date, WORD time,
                         stfs_write_file_t *file);
FRESULT stfs_write_file(stfs_write_file_t *file, const void *buffer,
                        UINT bytes_to_write, UINT *bytes_written);
FRESULT stfs_close_write_file(stfs_write_file_t *file, bool commit);
FRESULT stfs_mkdir(stfs_t *fs, const char *dir_path, const char *name,
                   BYTE attr, WORD date, WORD time);
FRESULT stfs_delete(stfs_t *fs, const char *path);
FRESULT stfs_delete_tree(stfs_t *fs, const char *path);

#endif  // STFS_H
