#include "3DO.h"
#include "MSC.h"
#include "CDFormat.h"
#include "pico/stdio.h"

#include "diskio.h"

#include <string.h>
#include <stdlib.h>


/* -------------------------------------------
- Thanks to https://github.com/trapexit/3dt for 3do image detection and filesystem extraction
---------------------------------------------*/

/* -------------------------------------------
  Everything in this file uses FatFs and/or TinyUSB, so it must only run on
  core 0. Requests coming from the 3DO (core 1) go through runOnCore0().
---------------------------------------------*/

#define PLAYLIST_MAX 16
typedef struct playlist_entry_s{
  char *path[PLAYLIST_MAX];
  int tocid[PLAYLIST_MAX];
  int nb_entries;
  int current_entry;
} playlist_entry;

playlist_entry playlist = {0};

bool onBootIso = false;

#define TOC_NAME_LIMIT 128

#if CFG_TUH_MSC
static bool check_eject(uint8_t dev_addr);
static void check_block(uint8_t dev_addr);
static void check_subq(uint8_t dev_addr);
#endif

static int current_toc = 0x00000000;
static int current_toc_level = 0;
static int current_toc_offset = 0;

//--------------------------------------------------------------------+
// Image files (one per track at most, as in multi FILE .cue sheets)
//--------------------------------------------------------------------+
static char *binPaths[MAX_TRACKS] = {0};
static int nbBinFiles = 0;
static int openBinIdx = -1;
static FIL curBinFile;
static FSIZE_t last_pos = 0;
#define INVALID_POS ((FSIZE_t)-1)

static void closeBinFile(void) {
  if (openBinIdx >= 0) f_close(&curBinFile);
  openBinIdx = -1;
  last_pos = INVALID_POS;
}

static void clearBinFiles(void) {
  closeBinFile();
  for (int i = 0; i < MAX_TRACKS; i++) {
    if (binPaths[i] != NULL) free(binPaths[i]);
    binPaths[i] = NULL;
  }
  nbBinFiles = 0;
}

static bool openBinFile(int idx) {
  if (idx == openBinIdx) return true;
  closeBinFile();
  if ((idx < 0) || (idx >= nbBinFiles) || (binPaths[idx] == NULL)) return false;
  if (f_open(&curBinFile, binPaths[idx], FA_READ) != FR_OK) {
    LOG_SATA("Can not open %s\n", binPaths[idx]);
    return false;
  }
  openBinIdx = idx;
  last_pos = 0;
  return true;
}

// Release the menu directory context of currentImage
static void releaseImageContext(void) {
  if (currentImage.curDir != NULL) {
    f_closedir(&currentImage.curDir->dir);
    free(currentImage.curDir);
  }
  if (currentImage.curPath != NULL) free(currentImage.curPath);
  currentImage.curDir = NULL;
  currentImage.curPath = NULL;
  current_toc_level = 0;
  current_toc = 0x00000000;
  current_toc_offset = 0;
}

// Forget the current image: menu context and image files
void MSC_releaseCurrentImage(void) {
  releaseImageContext();
  clearBinFiles();
  currentImage.dev_addr = 0xFF;
  currentImage.lun = 0x0;
}

static char* duplicate(const char *s) {
  if (s == NULL) return NULL;
  size_t l = strlen(s) + 1;
  char *r = malloc(l);
  if (r != NULL) memcpy(r, s, l);
  return r;
}

// Returns a newly allocated "dir\name" string
static char* joinPath(const char *dir, const char *name) {
  if ((dir == NULL) || (name == NULL)) return NULL;
  size_t l = strlen(dir) + strlen(name) + 2;
  char *r = malloc(l);
  if (r != NULL) snprintf(r, l, "%s\\%s", dir, name);
  return r;
}

// Returns a newly allocated copy of the directory part of path
static char* dirOf(const char *path) {
  const char *sep = strrchr(path, '\\');
  const char *sep2 = strrchr(path, '/');
  if ((sep == NULL) || ((sep2 != NULL) && (sep2 > sep))) sep = sep2;
  size_t len;
  if (sep != NULL) {
    len = sep - path;
  } else {
    const char *colon = strchr(path, ':');
    len = (colon != NULL) ? (size_t)(colon - path + 1) : 0;
  }
  char *r = malloc(len + 1);
  if (r == NULL) return NULL;
  memcpy(r, path, len);
  r[len] = 0;
  return r;
}

static void lbaToMsf(uint32_t lba, uint8_t *msf) {
  lba += 150;
  msf[0] = lba/(60*75);
  lba %= 60*75;
  msf[1] = lba / 75;
  msf[2] = lba % 75;
}

int getPlaylistEntries() {
  return playlist.nb_entries;
}

void updatePlayListEntries() {
  if (getPlaylistEntries() != 0) {
    if (playlist.current_entry >= playlist.nb_entries) {
      LOG_SATA("Playlist done - clearing\n");
      clearPlaylist();
      releaseImageContext();
    }
  }
}

bool MSC_Host_loop(device_s *dev)
{
#if CFG_TUH_MSC
  if (!usb_cmd_on_going) {
    switch(dev->state) {
      case EJECTING:
        check_eject(dev->dev_addr);
        return true;
      case MOUNTED:
        check_block(dev->dev_addr);
        check_subq(dev->dev_addr);
        return true;
      default:
        return false;
    }
  }
#endif
  return false;
}


#if CFG_TUH_MSC

static bool validateFile(FILINFO* fileInfo);
static bool getNextValidToc(FILINFO *fileInfo);

static bool check_eject(uint8_t dev_addr) {
  //Execute right now
  if (dev_addr == currentImage.dev_addr) return false;
  LOG_SATA("Eject\n");
  return true;
}

// Track physically holding this LBA (its pregap included)
static int trackForLba(uint32_t lba) {
  int t = 0;
  for (int i = 0; i < currentImage.nb_track; i++) {
    if (currentImage.tracks[i].start_lba <= (int32_t)lba) t = i;
  }
  return t;
}

static void check_subq(uint8_t dev_addr) {
  if (currentImage.dev_addr == 0xFF) return;
  if (subqRequired && (dev_addr == currentImage.dev_addr)) {
    uint8_t *buf = buffer_subq;
    subqRequired = false;
    if ((buf != NULL) && (currentImage.nb_track > 0)) {
      uint32_t start = start_Block;
      track_s *t = &currentImage.tracks[trackForLba(start)];
      memset(buf, 0x0, 16);
      uint8_t msf[3];
      lbaToMsf(start, msf);
      buf[9] = msf[0];
      buf[10] = msf[1];
      buf[11] = msf[2];
      uint32_t rel = (start >= t->lba) ? (start - t->lba) : 0;
      buf[5] = t->CTRL_ADR;
      buf[6] = t->id;
      buf[7] = (start >= t->lba) ? 1 : 0; // index 0 in the pregap
      buf[13] = rel/(60*75);
      rel %= 60*75;
      buf[14] = rel / 75;
      buf[15] = rel % 75;
    }
    read_done = true;
  }
}

void printPlaylist(void) {
  LOG_SATA("Playlist:\n");
  for (int i =0; i<playlist.nb_entries; i++) {
    LOG_SATA("%d: %s\n", i, playlist.path[i]);
  }
}

void clearPlaylist(void) {
  LOG_SATA("Clear playlist!\n");
  for (int i=0; i<PLAYLIST_MAX; i++) {
    if (playlist.path[i] != NULL) {
      free(playlist.path[i]);
      playlist.path[i] = NULL;
    }
  }
  playlist.nb_entries = 0;
  playlist.current_entry = 0;
}


void addToPlaylist(int entry, bool *valid, bool *added) {
  char *entry_path = NULL;
  *valid = false;
  *added = false;
  LOG_SATA("Add to playlist %x\n", entry);
  if (playlist.nb_entries >= PLAYLIST_MAX) return;
  entry_path = getPathForTOC(entry);
  if (entry_path == NULL) return;
  *valid = true;
  *added = true;
  playlist.tocid[playlist.nb_entries] = entry;
  playlist.path[playlist.nb_entries++] = entry_path;
  LOG_SATA("Add to playlist %s %d\n", entry_path, playlist.nb_entries);
}

// Offset of the user data inside a raw sector
static uint32_t headerSkip(const track_s *t, uint32_t out_size) {
  if ((t->CTRL_ADR & 0x4) == 0) return 0;  // Audio: always raw
  if (t->sector_size != 2352) return 0;    // Cooked image: nothing to skip
  if (out_size > 2048) return 0;           // Raw read requested
  if (t->mode == MODE_2) return 12 + 4 + 8; // Mode 2 Form 1: sync, header, subheader
  return 12 + 4;                            // Mode 1: sync, header
}

static void check_block(uint8_t dev_addr) {
  if (currentImage.dev_addr == 0xFF) return;
  if (!(blockRequired && (dev_addr == currentImage.dev_addr))) return;
  usb_cmd_on_going = true;
  blockRequired = false;

  bool ok = (currentImage.nb_track > 0);
  uint32_t out_size = currentImage.block_size_read;
  uint8_t *buffer = buffer_Block;
  uint32_t start = start_Block;
  uint32_t nb = nb_block_Block;

  for (uint32_t b = 0; ok && (b < nb); b++) {
    uint32_t lba = start + b;
    const track_s *t = &currentImage.tracks[trackForLba(lba)];
    uint8_t *dst = &buffer[b * out_size];
    if (((int64_t)lba < t->file_lba) || !openBinFile(t->file_idx)) {
      ok = false;
      break;
    }
    uint32_t skip = headerSkip(t, out_size);
    FSIZE_t pos = (FSIZE_t)((int64_t)lba - t->file_lba) * t->sector_size + skip;
    UINT copy = t->sector_size - skip;
    if (copy > out_size) copy = out_size;
    if (last_pos != pos) {
      if (f_lseek(&curBinFile, pos) != FR_OK) {
        LOG_SATA("Can not seek %s\n", binPaths[t->file_idx]);
        last_pos = INVALID_POS;
        ok = false;
        break;
      }
    }
    UINT read_nb = 0;
    if (f_read(&curBinFile, dst, copy, &read_nb) != FR_OK) {
      LOG_SATA("Can not read %s\n", binPaths[t->file_idx]);
      last_pos = INVALID_POS;
      ok = false;
      break;
    }
    last_pos = pos + read_nb;
    // Missing bytes (subcode, C2 or end of file) are reported as 0
    if (read_nb < out_size) memset(&dst[read_nb], 0, out_size - read_nb);
  }
  if (!ok) set3doDriveError();
  usb_cmd_on_going = false;
  read_done = true;
}

static bool is_3do_iso(uint8_t* buf) {
  uint8_t VOLUME_SYNC_BYTES[] = {0x5A,0x5A,0x5A,0x5A,0x5A};

  if(buf[0] != 0x01)
    return false;

  if(memcmp(&buf[1],&VOLUME_SYNC_BYTES[0],sizeof(VOLUME_SYNC_BYTES)) != 0)
    return false;

  if(buf[6] != 0x01)
    return false;

  return true;
}


static bool is_mode1_2352(uint8_t* buf) {
  uint8_t MODE1_SYNC_PATTERN[] = {0x00,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0x00};
  if(memcmp(&buf[0],&MODE1_SYNC_PATTERN[0],sizeof(MODE1_SYNC_PATTERN))!= 0)
    return false;

  if(buf[0x0F] != 0x01)
    return false;

  return is_3do_iso(&buf[0x10]);
}

static FIL probeFile; // static: keeps the large FIL object off the small stack

static bool isA3doImage(const char * path) {
  uint8_t buf[0x20];
  UINT read_nb = 0;
  bool ret = false;
  if (path == NULL) return false;
  if (f_open(&probeFile, path, FA_READ) != FR_OK) {
    return false;
  }

  if ((f_read(&probeFile, &buf, 0x20, &read_nb) == FR_OK) && (read_nb == 0x20)) {
    ret = ((is_mode1_2352(&buf[0])) || (is_3do_iso(&buf[0])));
  }
  f_close(&probeFile);
  return ret;
}

//--------------------------------------------------------------------+
// CUE sheet parsing
//--------------------------------------------------------------------+
static FIL cueFile;
static char cueLine[256];
static char cueKey[16];
static char cueRest[240];
static char cueName[240];
static track_s cueTracks[MAX_TRACKS];
static char *cueFiles[MAX_TRACKS];

static void freeCueFiles(int nb) {
  for (int i = 0; i < nb; i++) {
    if (cueFiles[i] != NULL) free(cueFiles[i]);
    cueFiles[i] = NULL;
  }
}

// Parse a cue sheet. When load is true and the sheet is valid, currentImage
// and the image file list are updated. Supports one or several FILE entries.
static bool parseCue(const char *cuePath, bool load) {
  bool valid = true;
  int nbTracks = 0;
  int nbFiles = 0;
  FSIZE_t fileSize = 0;
  uint16_t fileSectorSize = 0;
  uint32_t fileStartLba = 0; // position of the current FILE, in sectors from the first FILE
  bool hasIndex1[MAX_TRACKS] = {0};

  if (f_open(&cueFile, cuePath, FA_READ) != FR_OK) {
    LOG_SATA("can not open %s for reading\n", cuePath);
    return false;
  }
  char *dir = dirOf(cuePath);
  if (dir == NULL) {
    f_close(&cueFile);
    return false;
  }
  memset(cueTracks, 0, sizeof(cueTracks));
  memset(cueFiles, 0, sizeof(cueFiles));

  while (valid && f_gets(cueLine, sizeof(cueLine), &cueFile)) {
    cueRest[0] = 0;
    char *line = cueLine;
    // Skip an UTF-8 byte order mark
    if (((uint8_t)line[0] == 0xEF) && ((uint8_t)line[1] == 0xBB) && ((uint8_t)line[2] == 0xBF)) line += 3;
    int n = sscanf(line, " %15s %239[^\r\n]", cueKey, cueRest);
    if (n < 1) continue; // empty line

    if (strcmp(cueKey, "FILE") == 0) {
      if ((n < 2) || (nbFiles >= MAX_TRACKS)) { valid = false; break; }
      if ((sscanf(cueRest, " \"%239[^\"]\"", cueName) != 1) &&
          (sscanf(cueRest, " %239s", cueName) != 1)) { valid = false; break; }
      // The previous file ends where this one starts
      if (nbFiles > 0) {
        if (fileSectorSize == 0) { valid = false; break; } // FILE without TRACK
        fileStartLba += fileSize / fileSectorSize;
      }
      char *path = joinPath(dir, cueName);
      FILINFO binInfo;
      if ((path == NULL) || (f_stat(path, &binInfo) != FR_OK) || (binInfo.fattrib & AM_DIR)) {
        if (path != NULL) free(path);
        valid = false;
        break; //Bin file does not exist or is not a file
      }
      cueFiles[nbFiles++] = path;
      fileSize = binInfo.fsize;
      fileSectorSize = 0;
      continue;
    }

    if (strcmp(cueKey, "TRACK") == 0) {
      unsigned int track_num = 0;
      char type[16];
      if ((nbFiles == 0) || (nbTracks >= MAX_TRACKS) ||
          (sscanf(cueRest, " %u %15s", &track_num, type) != 2) ||
          (track_num < 1) || (track_num > MAX_TRACKS)) {
        valid = false;
        break;
      }
      track_s *t = &cueTracks[nbTracks];
      uint16_t size = 2352;
      char *slash = strchr(type, '/');
      if (slash != NULL) size = atoi(slash + 1);
      if (strncmp(type, "MODE1", 5) == 0) {
        t->CTRL_ADR = 0x4;
        t->mode = MODE_1;
      } else if (strncmp(type, "MODE2", 5) == 0) {
        //PhotoCD cue file
        t->CTRL_ADR = 0x4;
        t->mode = MODE_2;
      } else if (strncmp(type, "AUDIO", 5) == 0) {
        t->CTRL_ADR = 0x0;
        t->mode = CDDA;
        size = 2352;
      } else {
        valid = false;
        break;
      }
      if ((size != 2048) && (size != 2352)) { valid = false; break; }
      // All the tracks of one file share the same sector size
      if ((fileSectorSize != 0) && (fileSectorSize != size)) { valid = false; break; }
      fileSectorSize = size;
      t->id = track_num;
      t->sector_size = size;
      t->file_idx = nbFiles - 1;
      t->file_lba = fileStartLba;
      t->start_lba = INT32_MAX; // set by INDEX 00 or INDEX 01
      // The first track has to be a 3DO volume (except for Photo-CD)
      if ((nbTracks == 0) && (t->mode == MODE_1) && !isA3doImage(cueFiles[nbFiles - 1])) {
        valid = false;
        break;
      }
      nbTracks++;
      continue;
    }

    if (strcmp(cueKey, "INDEX") == 0) {
      unsigned int indexnum, min, sec, frame;
      if ((nbTracks == 0) || (sscanf(cueRest, " %u %u:%u:%u", &indexnum, &min, &sec, &frame) != 4) ||
          (sec >= 60) || (frame >= 75)) {
        valid = false;
        break;
      }
      track_s *t = &cueTracks[nbTracks - 1];
      // INDEX positions are relative to the start of the current FILE
      uint32_t pos = fileStartLba + (min*60 + sec)*75 + frame;
      if (indexnum == 0) {
        t->start_lba = pos;
      } else if (indexnum == 1) {
        t->lba = pos;
        if (t->start_lba == INT32_MAX) t->start_lba = pos;
        hasIndex1[nbTracks - 1] = true;
      }
      continue;
    }
    // Other commands (REM, PREGAP, POSTGAP, FLAGS...) are ignored
  }
  f_close(&cueFile);
  free(dir);

  if (valid && (nbTracks == 0)) valid = false;
  for (int i = 0; valid && (i < nbTracks); i++) {
    if (!hasIndex1[i]) valid = false;
  }
  if (!valid || !load) {
    freeCueFiles(nbFiles);
    return valid;
  }

  // The INDEX 01 of the first track is the LBA 0 of the disc: what is before
  // it in the file is the pregap of the first track (negative LBAs).
  uint32_t base = cueTracks[0].lba;
  for (int i = 0; i < nbTracks; i++) {
    cueTracks[i].lba -= base;
    cueTracks[i].start_lba -= (int32_t)base;
    cueTracks[i].file_lba -= (int32_t)base;
    lbaToMsf(cueTracks[i].lba, cueTracks[i].msf);
  }

  // Valid sheet: install it as the current image
  clearBinFiles();
  for (int i = 0; i < nbFiles; i++) {
    binPaths[i] = cueFiles[i];
    cueFiles[i] = NULL;
  }
  nbBinFiles = nbFiles;
  memcpy(currentImage.tracks, cueTracks, sizeof(track_s) * nbTracks);
  currentImage.nb_track = nbTracks;
  currentImage.first_track = cueTracks[0].id;
  currentImage.last_track = cueTracks[nbTracks - 1].id;
  currentImage.hasOnlyAudio = true;
  currentImage.format = 0x0;
  currentImage.multiSession = false;
  currentImage.offset = 0;
  for (int i = 0; i < nbTracks; i++) {
    if (cueTracks[i].CTRL_ADR & 0x4) currentImage.hasOnlyAudio = false;
    if (cueTracks[i].mode == MODE_2) currentImage.format = 0x20; //XA format
  }
  // Size of the main data sector of the image
  currentImage.block_size = cueTracks[0].sector_size;
  currentImage.block_size_read = (cueTracks[0].CTRL_ADR & 0x4) ? 2048 : 2352;
  currentImage.nb_block = fileStartLba + fileSize / fileSectorSize - base;
  lbaToMsf(currentImage.nb_block, currentImage.msf);
  return true;
}

static bool LoadfromCue(const char *filePath) {
  if (currentImage.dev_addr == 0xFF) return false;
  LOG_SATA("Load From Cue %s\n", filePath);
  return parseCue(filePath, true);
}

static bool ValidateInfofromCue(FILINFO *fileInfo) {
  if ((currentImage.dev_addr == 0xFF) || (currentImage.curPath == NULL)) return false;
  char *newPath = joinPath(currentImage.curPath, fileInfo->fname);
  if (newPath == NULL) return false;
  LOG_SATA("Validate Game %s\n", newPath);
  bool valid = parseCue(newPath, false);
  free(newPath);
  return valid;
}

static bool ValidateInfofromIso(FILINFO *fileInfo) {
  if ((currentImage.dev_addr == 0xFF) || (currentImage.curPath == NULL)) return false;
  char *newPath = joinPath(currentImage.curPath, fileInfo->fname);
  if (newPath == NULL) return false;
  bool ret = isA3doImage(newPath);
  free(newPath);
  return ret;
}

// Install a single track ISO image as currentImage. Takes ownership of path.
static bool setupIsoImage(char *path, FSIZE_t fsize) {
  uint16_t sector_size;
  if ((fsize % 2352)==0) {
    sector_size = 2352;
  } else if ((fsize % 2048)==0) {
    sector_size = 2048;
  } else {
    //Bad format
    LOG_SATA("File is %lu bytes length\n", (unsigned long)fsize);
    free(path);
    return false;
  }
  clearBinFiles();
  binPaths[0] = path;
  nbBinFiles = 1;

  // The data sent to the 3DO is always the 2048 bytes of user data by default,
  // the sync/header of raw 2352 images is skipped by check_block.
  currentImage.block_size = sector_size;
  currentImage.block_size_read = 2048;
  currentImage.offset = 0;
  currentImage.multiSession = false;

  currentImage.nb_block = fsize / sector_size;

  currentImage.first_track = 1;
  currentImage.last_track = 1;
  currentImage.nb_track = 1;
  currentImage.hasOnlyAudio = false;
  currentImage.format = 0x0; //MODE1 always

  memset(&currentImage.tracks[0], 0, sizeof(track_s));
  currentImage.tracks[0].CTRL_ADR = 0x4;
  currentImage.tracks[0].msf[0] = 0;
  currentImage.tracks[0].msf[1] = 2;
  currentImage.tracks[0].msf[2] = 0;
  currentImage.tracks[0].lba = 0;
  currentImage.tracks[0].mode = MODE_1;
  currentImage.tracks[0].id = 1;
  currentImage.tracks[0].sector_size = sector_size;
  currentImage.tracks[0].file_idx = 0;
  currentImage.tracks[0].file_lba = 0;
  currentImage.tracks[0].start_lba = 0;

  lbaToMsf(currentImage.nb_block, currentImage.msf);
  return true;
}

static bool LoadfromIso(const char *filePath) {
  if (currentImage.dev_addr == 0xFF) return false;
  FILINFO fileInfo;
  if (!isA3doImage(filePath)) {
    return false;
  }
  if (f_stat(filePath, &fileInfo) != FR_OK) return false;

  LOG_SATA("Load Game %s\n", filePath);
  // The playlist keeps its own copy of the path
  char *path = duplicate(filePath);
  if (path == NULL) return false;
  return setupIsoImage(path, fileInfo.fsize);
}


// On success currentImage takes ownership of curPath
static bool extractBootImage(FILINFO *fileInfo, char* curPath, uint8_t dev_addr) {
  if (currentImage.dev_addr != 0xFF) return false;
  device_s *dev = getDevice(dev_addr);
  if (dev == NULL) return false;
  char *newPath = joinPath(curPath, fileInfo->fname);
  if (newPath == NULL) return false;
  if (!isA3doImage(newPath)) {
    LOG_SATA("Is not a 3DO image %s\n", curPath);
    free(newPath);
    return false;
  }
  if (!setupIsoImage(newPath, fileInfo->fsize)) {
    return false;
  }

  currentImage.dev_addr = dev->dev_addr;
  currentImage.lun = dev->lun;

  if ((currentImage.curPath != NULL) && (currentImage.curPath != curPath)) free(currentImage.curPath);
  currentImage.curPath = curPath;
  return true;
}

static bool validateFile(FILINFO* fileInfo) {
  if (f_path_contains(fileInfo->fname,"*.cue")){
     return ValidateInfofromCue(fileInfo);
  }
  if (f_path_contains(fileInfo->fname,"*.iso")){
    return ValidateInfofromIso(fileInfo);
  }
  return false;
}

static bool loadFile(const char* filepath) {
  if (f_path_contains(filepath,"*.cue")){
     return LoadfromCue(filepath);
  }
  if (f_path_contains(filepath,"*.iso")){
    return LoadfromIso(filepath);
  }
  return false;
}

static dir_t* getNewDir(void) {
  dir_t * ret=(dir_t*)malloc(sizeof(dir_t));
  if (ret != NULL) memset(ret, 0, sizeof(dir_t));
  return ret;
}

// Device address from a FatFs path ("N:..." is the device N+1)
static uint8_t devAddrFromPath(const char *path) {
  if ((path == NULL) || (path[0] < '0') || (path[0] > '9')) return 0xFF;
  return (uint8_t)(atoi(path) + 1);
}

static bool loadPlaylistEntry(uint8_t dev_addr) {
  (void) dev_addr;
  bool ret = false;
  int entry = playlist.tocid[playlist.current_entry];
  char *path = playlist.path[playlist.current_entry];
  LOG_SATA("try to load (%x) %d %s\n", entry, playlist.current_entry, path);
  if (getTocEntry(entry) == 0x0) {
    //Entry detected as device (a CD drive)
    LOG_SATA("Detected an entry\n");
    device_s *new_dev = getDevice(entry>>24);
    if ((new_dev != NULL) && (new_dev->type == CD_TYPE) && (new_dev->rawImage.nb_track > 0)) {
      releaseImageContext();
      clearBinFiles();
      memcpy(&currentImage, &new_dev->rawImage, sizeof(cd_s));
      currentImage.curDir = NULL;
      currentImage.curPath = NULL;
      new_dev->state = MOUNTED;
      usb_cmd_on_going = false;
      set3doCDReady(new_dev->dev_addr, true);
      set3doDriveMounted(new_dev->dev_addr, true);
      ret = true;
    } else {
      LOG_SATA("CD drive is not available anymore\n");
    }
  } else {
    LOG_SATA("Try to load\n");
    device_s *new_dev = getDevice(devAddrFromPath(path));
    if (new_dev != NULL) {
      currentImage.dev_addr = new_dev->dev_addr;
      currentImage.lun = new_dev->lun;
      if (loadFile(path) && openBinFile(0)) {
        LOG_SATA("Game loaded %d\n",playlist.current_entry);
        new_dev->state = MOUNTED;
        usb_cmd_on_going = false;
        set3doCDReady(currentImage.dev_addr, true);
        set3doDriveMounted(currentImage.dev_addr, true);
        ret = true;
      } else {
        LOG_SATA("Can not open the Game!\n");
        currentImage.dev_addr = 0xFF;
        currentImage.lun = 0;
      }
    }
  }
  // The entry is consumed even if it fails, otherwise it would be retried forever
  playlist.current_entry++;
  updatePlayListEntries();

  return ret;
}

static bool loadBootIso(uint8_t dev_addr) {
  bool ret = false;
  FRESULT res;
  FILINFO fileInfo;
  LOG_SATA("Load boot.iso\n");
  device_s *dev = getDevice(dev_addr);
  if (dev == NULL) return false;
  dir_t *curDir = getNewDir();
  char *curPath = (char *) malloc(8);
  if ((curDir == NULL) || (curPath == NULL)) {
    free(curDir);
    free(curPath);
    return false;
  }
  snprintf(curPath, 8, "%d:", dev_addr-1);
  LOG_SATA("Try to load on %s\n", curPath);
  res = f_findfirst(&curDir->dir, &fileInfo, curPath, "boot.iso");
  if ((res != FR_OK) || (strlen(fileInfo.fname) == 0)) {
    //report error. Boot iso is not found
    LOG_SATA("Error on %s\n", curPath);
  } else if (extractBootImage(&fileInfo, curPath, dev_addr)) {
    // currentImage owns curPath now
    if (currentImage.curDir != NULL) free(currentImage.curDir);
    currentImage.curDir = curDir;
    if (openBinFile(0)) {
      dev->state = MOUNTED;
      onBootIso = true;
      usb_cmd_on_going = false;
      // Fresh menu context on the boot device
      current_toc_level = 0;
      current_toc = 0x00000000;
      current_toc_offset = 0;
      LOG_SATA("Boot iso path %s\n", currentImage.curPath);
      set3doCDReady(currentImage.dev_addr, true);
      set3doDriveMounted(currentImage.dev_addr, true);
      ret = true;
    } else {
      LOG_SATA("Can not open the Game!\n");
      // Frees curDir and curPath which are owned by currentImage
      releaseImageContext();
      clearBinFiles();
      currentImage.dev_addr = 0xFF;
      currentImage.lun = 0;
      return false;
    }
  } else {
    LOG_SATA("Can not extract on %s\n", curPath);
  }
  if (!ret) {
    free(curDir);
    free(curPath);
  }
  return ret;
}

bool loadBootImage(uint8_t dev_addr) {
  device_s *dev = getDevice(dev_addr);
  if ((dev == NULL) || (dev->type != MSC_TYPE) || !dev->isFatFs) return false;
  device_s *cur = getDevice(currentImage.dev_addr);
  if ((cur != NULL) && (cur->type == MSC_TYPE) && (currentImage.dev_addr != dev_addr)) {
    // The image is on another device: nothing to do from this one
    LOG_SATA("Boot image is on %d\n", currentImage.dev_addr);
    return false;
  }
  LOG_SATA("Handle Boot image (%d)\n", playlist.nb_entries);
  if (playlist.nb_entries != 0) {
    LOG_SATA("Handle playlist\n");
    return loadPlaylistEntry(dev_addr);
  } else {
    LOG_SATA("Handle boot.iso\n");
    return loadBootIso(dev_addr);
  }
}

bool MSC_Inquiry(uint8_t dev_addr, uint8_t lun) {
  FRESULT result;
  LOG_SATA("MSC_Inquiry %d\n", dev_addr);
  if (tuh_msc_get_block_size(dev_addr, lun) == 0) {
    LOG_SATA("MSC block is 0\n");
    return false;
  }
  device_s *dev = getDevice(dev_addr);
  if (dev == NULL) return false;
  char path[8];
  snprintf(path, sizeof(path), "%d:", dev_addr-1);
  LOG_SATA("Try to mount on %s\n", path);
  result = f_mount(&dev->DiskFATState, path , 1);
  if (result!=FR_OK) {
    LOG_SATA("Can not mount\n");
    dev->isFatFs = false;
    dev->useable = false;
    return false;
  }
  dev->isFatFs = true;
  dev->useable = true;
  LOG_SATA("MSC mounted here: %s\n", path);
  mediaInterrupt();
  return true;
}

int getTocLevel(void) {
  return current_toc_level;
}

int getTocIndex(void) {
  return ((unsigned int)current_toc)>>24;
}

int getTocEntry(int index) {
  return (index&0xFFFFFF);
}

static bool isUp(int index) {
  return (getTocEntry(index)==0xFFFFFF);
}

void setTocLevel(int index) {
  if (currentImage.dev_addr == 0xFF) return;
  FILINFO fileInfo;
  LOG_SATA("current_toc %d (%d) vs %d (%d) current_toc_level %d\n", getTocEntry(current_toc), getTocIndex(), getTocEntry(index), ((unsigned int)index)>>24, current_toc_level);
  if (isUp(index)) {
    if (currentImage.curPath == NULL) return;
    LOG_SATA("Set Toc Level start %s level %d\n", currentImage.curPath, current_toc_level);
    if ((current_toc_level == 1)&&(getDeviceCount()>1)) {
      // Back to the device list
      if (currentImage.curDir != NULL) {
        f_closedir(&currentImage.curDir->dir);
        free(currentImage.curDir);
        currentImage.curDir = NULL;
      }
      free(currentImage.curPath);
      currentImage.curPath = NULL;
      current_toc_level = 0;
      current_toc = 0x00000000;
    } else {
      char *lastDir = strrchr(currentImage.curPath, '\\');
      if ((lastDir != NULL) && (currentImage.curDir != NULL)) {
        char *newPath = malloc(lastDir - currentImage.curPath + 1);
        if (newPath == NULL) return;
        memcpy(newPath, currentImage.curPath, lastDir - currentImage.curPath);
        newPath[lastDir-currentImage.curPath] = 0;
        f_closedir(&currentImage.curDir->dir);
        f_opendir(&currentImage.curDir->dir, newPath);
        current_toc_offset = 0;
        free(currentImage.curPath);
        currentImage.curPath = newPath;
        LOG_SATA("Set Toc Level path %s\n", currentImage.curPath);
        current_toc &= ~0xFFFFFF;
        if (current_toc_level > 0) current_toc_level -= 1;
      }
    }
    LOG_SATA("New current_toc_level %d\n", current_toc_level);
    return;
  }
  if (current_toc != index) {
    //need to change the current TOC level
    //Get required Toc Entry
    if (((getTocEntry(index)) == 0) && (current_toc_level == 0)){
      int idx = (((unsigned int)index)>>24)-1;
      if (idx < 0) idx = 0;
      dir_t *newDir = getNewDir();
      char *newPath = (char*)malloc(20 * sizeof(char));
      if ((newDir == NULL) || (newPath == NULL)) {
        free(newDir);
        free(newPath);
        return;
      }
      // Release the previous context before replacing it
      if (currentImage.curDir != NULL) {
        f_closedir(&currentImage.curDir->dir);
        free(currentImage.curDir);
      }
      if (currentImage.curPath != NULL) free(currentImage.curPath);
      currentImage.curDir = newDir;
      currentImage.curPath = newPath;
      snprintf(currentImage.curPath, 20, "%d:", idx);
      f_opendir(&currentImage.curDir->dir, currentImage.curPath);
      current_toc_offset = 0;
      current_toc_level++;
      LOG_SATA("Setup root path to %s\n", currentImage.curPath);
    } else {
      if ((currentImage.curDir == NULL) || (currentImage.curPath == NULL)) return;
      int i = 0;
      LOG_SATA("Load path %s\n", currentImage.curPath);
      f_closedir(&currentImage.curDir->dir);
      f_opendir(&currentImage.curDir->dir, currentImage.curPath);
      current_toc_offset = 0;
      fileInfo.fname[0] = 0;
      while(i < getTocEntry(index)) {
        LOG_SATA("Look for entry %d\n", i);
        if (getNextValidToc(&fileInfo)) {
          i++;
          current_toc_offset++;
        } else if (fileInfo.fname[0] == 0) {
          //Should raise en arror. Shall never happen
          LOG_SATA("!!! WTF not found\n");
          return; //End of file list
        }
      }
      LOG_SATA("Finish at entry %d\n", i);
      if (fileInfo.fattrib & AM_DIR) {
        char *newPath = joinPath(currentImage.curPath, fileInfo.fname);
        if (newPath == NULL) return;
        free(currentImage.curPath);
        currentImage.curPath = newPath;
        LOG_SATA("Set Toc Level path  to dir %s\n", currentImage.curPath);
        f_closedir(&currentImage.curDir->dir);
        f_opendir(&currentImage.curDir->dir, currentImage.curPath);
        current_toc_offset = 0;
        current_toc_level++;
      } else {
        LOG_SATA("!!! WTF not a directory\n");
      }
    }
    current_toc = index;
  } else {
    if ((getTocLevel() == 0) && (getDeviceCount()<=1)) {
      current_toc = (currentImage.dev_addr)<<24;
    }
  }
  LOG_SATA("New current_toc_level %d\n", current_toc_level);
}

static bool getNextValidToc(FILINFO *fileInfo) {
  fileInfo->fname[0] = 0;
  if ((currentImage.dev_addr == 0xFF) || (currentImage.curDir == NULL)) return false;
  FRESULT res;
  res = f_readdir(&currentImage.curDir->dir, fileInfo);                   /* Read a directory item */
  if (res != FR_OK) fileInfo->fname[0] = 0;  /* Break on error or end of dir */
  if (fileInfo->fname[0] == 0) return false;  /* Break on error or end of dir */
  if ((fileInfo->fname[0] == '.') || (fileInfo->fattrib & (AM_HID | AM_SYS))) return false;
  if (strlen(fileInfo->fname) >= TOC_NAME_LIMIT) return false;
  if (!(fileInfo->fattrib & AM_DIR)) {
    if (!validateFile(fileInfo)) return false;
  }
  LOG_SATA("File %s is ok\n", fileInfo->fname);
  return true;
}

bool seekTocTo(int index) {
  if ((currentImage.dev_addr == 0xFF) || (currentImage.curDir == NULL) || (currentImage.curPath == NULL)) return false;
  FILINFO fileInfo;
  int i = 0;
  f_closedir(&currentImage.curDir->dir);
  if (f_opendir(&currentImage.curDir->dir, currentImage.curPath) != FR_OK) return false;
  current_toc_offset = 0;
  while(i < index) {
    if (getNextValidToc(&fileInfo)) {
      i++;
      current_toc_offset++;
    } else if (fileInfo.fname[0] == 0) {
      return false; //End of file list
    }
  }
  return true;
}

bool getReturnTocEntry(toc_entry* toc) {
  if (toc == NULL) return false;
  toc->flags = TOC_FLAG_DIR;
  toc->toc_id = 0xFFFFFFFF;
  toc->name_length = 3;
  toc->name = malloc(toc->name_length);
  if (toc->name == NULL) return false;
  snprintf(toc->name, toc->name_length, "..");
  return true;
}

bool getNextTOCEntry(toc_entry* toc) {
  FILINFO fileInfo;
  if (toc == NULL) return false;

  while (!getNextValidToc(&fileInfo)) {
    if (fileInfo.fname[0] == 0) return false;
  }
  current_toc_offset++;

  if (fileInfo.fattrib & AM_DIR) {                   /* It is a directory */
    toc->flags = TOC_FLAG_DIR;
  } else {                                       /* It is a file. */
    toc->flags = TOC_FLAG_FILE;
  }
  toc->toc_id = current_toc_offset;
  toc->name_length = strlen(fileInfo.fname) + 1;
  toc->name = malloc(toc->name_length);
  if (toc->name == NULL) return false;
  memcpy(toc->name, fileInfo.fname, toc->name_length);
  return true;
}

//--------------------------------------------------------------------+
// Data files for the ODE menu (saves, settings...)
//--------------------------------------------------------------------+
static FIL curDataFile;
static bool dataFileOpen = false;

bool closeDataFile(void) {
  if (!dataFileOpen) return true;
  dataFileOpen = false;
  return (f_close(&curDataFile) == FR_OK);
}

bool openDataFile(const char* name, uint16_t name_length, bool write) {
  closeDataFile();
  if ((currentImage.dev_addr == 0xFF) || (currentImage.curPath == NULL) || (name == NULL)) return false;
  // The name sent by the 3DO is not always 0 terminated
  size_t len = strnlen(name, name_length);
  if (len == 0) return false;
  size_t length = strlen(currentImage.curPath) + 1 + len + 1;
  char *path = malloc(length);
  if (path == NULL) return false;
  snprintf(path, length, "%s\\%.*s", currentImage.curPath, (int)len, name);
  BYTE mode = write ? (FA_WRITE | FA_READ | FA_OPEN_ALWAYS) : FA_READ;
  dataFileOpen = (f_open(&curDataFile, path, mode) == FR_OK);
  free(path);
  return dataFileOpen;
}

bool writeDataFile(const uint8_t* buf, uint16_t length, uint16_t *written) {
  UINT nb_write = 0;
  *written = 0;
  if (!dataFileOpen) return false;
  bool ok = (f_write(&curDataFile, buf, length, &nb_write) == FR_OK);
  ok &= (f_sync(&curDataFile) == FR_OK);
  *written = nb_write;
  return ok && (nb_write == length);
}

bool readDataFile(uint8_t* buf, uint16_t length, uint16_t *read) {
  UINT nb_read = 0;
  *read = 0;
  if (!dataFileOpen) return false;
  FRESULT res = f_read(&curDataFile, buf, length, &nb_read);
  if (res != FR_OK) LOG_SATA("Issue while reading %d (%d) => %x\n", length, nb_read, res);
  *read = nb_read;
  return (res == FR_OK);
}

#endif
