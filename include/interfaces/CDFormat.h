#ifndef __CDROM_FORMAT_H_INCLUDE__
#define __CDROM_FORMAT_H_INCLUDE__

#include "diskio.h"

typedef enum {
  MODE_0 = 0,
  MODE_1,
  MODE_2,
  CDDA
} mode_s;

typedef struct dir_s{
  DIR dir;
  int nbSubDir;
  struct dir_s *subDirs;
} dir_t;

typedef struct {
  uint8_t id;
  uint8_t CTRL_ADR;
  uint8_t msf[3];
  uint32_t lba;
  mode_s mode;
  uint16_t sector_size; // size of a sector in the image file (2048 or 2352)
  uint8_t file_idx;     // index of the image file holding this track
  int32_t file_lba;     // disc LBA matching the first sector of this file
  int32_t start_lba;    // first LBA of the track: INDEX 00 (pregap) if any, else lba
} track_s;

#define MAX_TRACKS 99

typedef struct {
  bool multiSession;
  uint8_t first_track;
  uint8_t last_track;
  uint8_t format;
  uint8_t nb_track;
  uint8_t msf[3];
  uint32_t nb_block;
  uint16_t block_size;
  uint16_t block_size_read;
  bool hasOnlyAudio;
  track_s tracks[100];
  uint8_t offset;
  uint8_t dev_addr;
  uint8_t lun;
  dir_t *curDir;
  char *curPath;
} cd_s;

extern cd_s currentImage;

#endif