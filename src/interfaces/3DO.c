#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/dma.h"
#include "hardware/sync.h"
#include "hardware/structs/bus_ctrl.h"
#include "pico/multicore.h"
#include "pico/critical_section.h"

#include "3DO.h"
#include "MSC.h"
#include "CDROM.h"
#include "CDFormat.h"

#define DATA_BUS_MASK (0xFF<<CDD0)

#ifdef DIR_IN_HI
#define DATA_IN 1
#define DATA_OUT 0
#else
#define DATA_IN 0
#define DATA_OUT 1
#endif

#include "read.pio.h"
#ifdef HW_HS_FC
#include "write_hs_fc.pio.h"
#else
#include "write.pio.h"
#endif

#define GET_BUS(A) (((A)>>CDD0)&0xFF)

extern cd_s currentImage;

typedef enum{
  SPIN_UP = 0x2,
  EJECT_DISC = 0x6,
  INJECT_DISC = 0x7,
  SET_MODE = 0x09,
  ABORT = 0x08,
  FLUSH = 0x0B,
  READ_DATA = 0x10,
  DATA_PATH_CHECK = 0x80,
  READ_ERROR = 0x82,
  READ_ID    = 0x83,
  READ_CAPACITY = 0x85,
  READ_SUB_Q = 0x87,
  READ_DISC_INFO = 0x8B,
  READ_TOC = 0x8C,
  READ_SESSION = 0x8D,
//Fixel added commands
  EXT_ID = 0x93,
  CHANGE_TOC = 0xC0,
  GET_TOC = 0xC1,
  GET_DESC = 0xC2,
  CLEAR_PLAYLIST = 0xC3,
  ADD_PLAYLIST = 0xC4,
  LAUNCH_PLAYLIST = 0xC5,

  GET_TOC_LIST = 0xD1,

  CREATE_FILE = 0xE0,
  OPEN_FILE = 0xE1,
  SEEK_FILE = 0xE2,
  READ_FILE_BYTE = 0xE3,
  WRITE_FILE_BYTE = 0xE4,
  CLOSE_FILE = 0xE5,
  WRITE_BUFFER_OFFSET = 0xE6,
  READ_BUFFER = 0xE7,

  UPDATE_ODE = 0xF0,
}CD_request_t;

/*
case 0xc0: dev_ode_changetoc(); break;
case 0xc1: dev_ode_gettoc(); break;
case 0xc2: dev_ode_getdesc(); break;
case 0xc3: dev_ode_clearpl(); break;
case 0xc4: dev_ode_addpl(); break;
case 0xc5: dev_ode_launchpl(); break;

case 0xd1: dev_ode_gettoclist(); break;


case 0xe0: dev_ode_createfile(); break;
case 0xe1: dev_ode_openfile(); break;
case 0xe2: dev_ode_seekfile(); break;
case 0xe3: dev_ode_readfile(); break;
case 0xe4: dev_ode_writefile(); break;
case 0xe5: dev_ode_closefile(); break;
case 0xe6: dev_ode_bufsend(); break;
case 0xe7: dev_ode_bufrecv(); break;

case 0xf0: dev_ode_startupdate(); break;
*/

typedef enum{
  TRAY_IN =	0x80,
  DISC_PRESENT = 0x40,
  SPINNING = 0x20,
  CHECK_ERROR = 0x10,
  DOUBLE_SPEED = 0x02,
  DISC_RDY = 0x01
}CD_status_t;

typedef enum{
  NO_ERROR = 0x0,
  SOFT_READ_RETRY = 0x1,
  SOFT_READ_CORRECTION = 0x2,
  NOT_READY = 0x3,
  NO_TOC = 0x4,
  HARD_READ = 0x5,
  SEEK_FAIL = 0x6,
  TRACKING_FAIL = 0x7,
  DRIVE_RAM_ERROR = 0x8,
  SELF_TEST_ERROR = 0x9,
  FOCUSING_FAIL = 0xA,
  SPINDLE_FAIL = 0xB,
  DATA_PATH_FAIL = 0xC,
  ILLEGAL_LBA = 0xD,
  ILLEGAL_CDB = 0xE,
  END_USER_TRACK = 0xF,
  ILLEGAL_MODE = 0x10,
  MEDIA_CHANGED = 0x11,
  POWER_OR_RESET_OCCURED = 0x12,
  DRIVE_ROM_FAIL = 0x13,
  ILLEGAL_CMD = 0x14,
  DISC_REMOVED = 0x15,
  HARDWARE_ERROR = 0x16,
  ILLEGAL_REQUEST = 0x17,
}CD_error_t;

typedef enum {
  CHAN_WRITE_STATUS = 0,
  CHAN_WRITE_DATA,
  CHAN_MAX
} DMA_chan_t;

// Maximum time to wait for the USB side before reporting a read error
#define READ_TIMEOUT_MS 10000

dma_channel_config config[CHAN_MAX];
uint8_t sm[CHAN_MAX];
uint8_t sm_offset[CHAN_MAX];
int channel[CHAN_MAX];

volatile int errorOnDisk = 0;
// Set when a read requested by the 3DO failed on the USB side
static volatile bool readError = false;

uint sm_read = -1;
uint instr_jmp_read;

uint instr_out;
uint instr_pull;
uint instr_set;
uint instr_jmp[CHAN_MAX];
uint instr_restart;

uint nbWord = 0;

static bool use_cdrom = false;

static bool ledState = false;

static volatile bool canHandleReset = false;

static int pitch = 0;

// status is updated by both cores: always modify it under the lock
static critical_section_t statusLock;
static volatile uint8_t errorCode = POWER_OR_RESET_OCCURED;
static volatile uint8_t status = TRAY_IN | CHECK_ERROR | DISC_RDY;

static void statusSet(uint8_t mask) {
  critical_section_enter_blocking(&statusLock);
  status |= mask;
  critical_section_exit(&statusLock);
}

static void statusClear(uint8_t mask) {
  critical_section_enter_blocking(&statusLock);
  status &= ~mask;
  critical_section_exit(&statusLock);
}

static void statusAssign(uint8_t value) {
  critical_section_enter_blocking(&statusLock);
  status = value;
  critical_section_exit(&statusLock);
}

static inline bool inReset(void) {
  return !gpio_get(CDRST);
}

//--------------------------------------------------------------------+
// Jobs executed on core 0 (TinyUSB and FatFs are not usable from core 1)
//--------------------------------------------------------------------+
static void closeTrayJob(void *arg) {
  bool close = *(bool *)arg;
  device_s *dev = NULL;
  bool has_usb = false;
  bool has_cd = false;
  for (int i = 0; i<CFG_TUH_DEVICE_MAX; i++) {
    device_s *curdev = getDeviceIndex(i);
    if (curdev->type == MSC_TYPE) {
      has_usb = true;
    }
    if (curdev->type == CD_TYPE) {
      has_cd = true;
    }
  }
  LOG_SATA("Ask to eject %d has_sub %d has_cd %d\n", close, has_usb, has_cd);
  //By default eject currentImage device
  if (currentImage.dev_addr != 0xFF) dev = getDevice(currentImage.dev_addr);
  //In case there is no currentImage, we might have an invalid CD
  if ((currentImage.dev_addr == 0xFF) && (has_cd)) {
    for (int i = 0; i<CFG_TUH_DEVICE_MAX; i++) {
      device_s *curdev = getDeviceIndex(i);
      if (curdev->type == CD_TYPE) {
        dev = curdev;
        break;
      }
    }
  }
  if (dev != NULL) {
    //If the currentImage device is a cd but we have usb key, do not eject it
    if ((dev->type == CD_TYPE) && (has_usb))
      dev = NULL;
  }
  //But In case we are on Boot menu and there is a CD, eject the CD
  if (onBootIso && has_cd) {
    //on Boot menu, if there is a CDROM, try to eject it.
    for (int i = 0; i<CFG_TUH_DEVICE_MAX; i++) {
      device_s *curdev = getDeviceIndex(i);
      if (curdev->type == CD_TYPE) {
        dev = curdev;
        break;
      }
    }
  }
  if (dev != NULL) {
    LOG_SATA("request Eject on dev %d, type %s\n", dev->dev_addr, (dev->type==CD_TYPE)?"CD":"USB");
    if (!USBDriveEject(dev->dev_addr, !close)) {
      LOG_SATA("Can not eject/inject\n");
      return;
    }
  }
  if (!onBootIso)
    mediaInterrupt();
}

void close_tray(bool close) {
  runOnCore0(closeTrayJob, &close);
}

void wait_out_of_reset() {
  while( !gpio_get(CDRST)) {
    gpio_put(CDMDCHG, 1); //Under reset
  }
  sleep_ms(500);
  gpio_put(CDMDCHG, 0); //Under reset
  sleep_ms(100);
  gpio_put(CDMDCHG, 1); //Under reset
}

void set3doCDReady(uint8_t dev_addr, bool on) {
  if (on && (currentImage.dev_addr == dev_addr)) {
    errorOnDisk = 0;
    readError = false;
    switch(currentImage.format) {
      case 0x0:
        if (currentImage.hasOnlyAudio)
          LOG_SATA("Audio CD detected\n");
        else
          LOG_SATA("Data CD detected\n");
          //Need to check if compatible 3DO game
        break;
      case 0x20:
        LOG_SATA("Photo-CD detected\n");
        break;
      case 0xFF:
        LOG_SATA("CD-i detected\n");
        break;
    }
    statusAssign(TRAY_IN | DISC_RDY | DISC_PRESENT | SPINNING);
    errorCode = 0;
  } else {
    if (on) LOG_SATA("Current Image is on dev addr %x but CD is %x\n", currentImage.dev_addr, dev_addr);
  }

}

// Core 0 only
void set3doDriveMounted(uint8_t dev_addr, bool on) {
  if ((currentImage.dev_addr == dev_addr) && (!on)) {
    LOG_SATA("set3doDriveMounted %d\n", on);
    MSC_releaseCurrentImage();
  }
}

void set3doDriveReady() {
}

// Can be called from a USB callback: the USB reset itself is deferred
void set3doDriveError() {
  LOG_SATA("Drive error\n");
  errorOnDisk = 1;
  readError = true;
  errorCode = SOFT_READ_RETRY;
  critical_section_enter_blocking(&statusLock);
  status |= CHECK_ERROR;
  status &= ~(DISC_RDY | SPINNING);
  critical_section_exit(&statusLock);
  USB_requestReset();
}

bool is3doData() {
  return pio_sm_get_rx_fifo_level(pio0, sm_read) != 0;
}

uint32_t get3doData() {
  uint32_t val = 0x0;
  val = pio_sm_get_blocking(pio0, sm_read);
  return val;
}

// Read the 6 parameter bytes following a command byte.
// Returns false if the 3DO has been reset meanwhile.
static bool get3doParams(unsigned int *data_in) {
  for (int i=0; i<6; i++) {
    while (!is3doData()) {
      if (inReset()) return false;
    }
    data_in[i] = GET_BUS(get3doData());
  }
  return true;
}

void print_dma_ctrl(dma_channel_hw_t *channel) {
    uint32_t ctrl = channel->ctrl_trig;
    int rgsz = (ctrl & DMA_CH0_CTRL_TRIG_RING_SIZE_BITS) >> DMA_CH0_CTRL_TRIG_RING_SIZE_LSB;
    LOG_SATA("(%08x) ber %d rer %d wer %d busy %d trq %d cto %d rgsl %d rgsz %d inw %d inr %d sz %d hip %d en %d",
           (uint) ctrl,
           ctrl & DMA_CH0_CTRL_TRIG_AHB_ERROR_BITS ? 1 : 0,
           ctrl & DMA_CH0_CTRL_TRIG_READ_ERROR_BITS ? 1 : 0,
           ctrl & DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS ? 1 : 0,
           ctrl & DMA_CH0_CTRL_TRIG_BUSY_BITS ? 1 : 0,
           (int) ((ctrl & DMA_CH0_CTRL_TRIG_TREQ_SEL_BITS) >> DMA_CH0_CTRL_TRIG_TREQ_SEL_LSB),
           (int) ((ctrl & DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS) >> DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB),
           ctrl & DMA_CH0_CTRL_TRIG_RING_SEL_BITS ? 1 : 0,
           rgsz ? (1 << rgsz) : 0,
           ctrl & DMA_CH0_CTRL_TRIG_INCR_WRITE_BITS ? 1 : 0,
           ctrl & DMA_CH0_CTRL_TRIG_INCR_READ_BITS ? 1 : 0,
           1 << ((ctrl & DMA_CH0_CTRL_TRIG_DATA_SIZE_BITS) >> DMA_CH0_CTRL_TRIG_DATA_SIZE_LSB),
           ctrl & DMA_CH0_CTRL_TRIG_HIGH_PRIORITY_BITS ? 1 : 0,
           ctrl & DMA_CH0_CTRL_TRIG_EN_BITS ? 1 : 0);
}

void setDataDir(int value) {
  gpio_put(DIR_DATA, value);
  if (value == DATA_IN) {
      pio_sm_drain_tx_fifo(pio0, sm_read);
  }
}


void restartPio(uint8_t channel) {
  pio_sm_drain_tx_fifo(pio0, sm[channel]);
  pio_sm_restart(pio0, sm[channel]);
  pio_sm_exec(pio0, sm[channel], instr_jmp[channel]);
  pio_sm_set_enabled(pio0, sm[channel], true);
}

static void restartReadPio() {
  pio_sm_drain_tx_fifo(pio0, sm_read);
  pio_sm_restart(pio0, sm_read);
  pio_sm_exec(pio0, sm_read, instr_jmp_read);
  pio_sm_set_enabled(pio0, sm_read, true);
}

void startDMA(uint8_t access, uint8_t *buffer, uint32_t nbWord) {
  dma_channel_transfer_from_buffer_now(channel[access], buffer, nbWord);
}

// Wait for the end of an answer, unless the 3DO is reset meanwhile
static bool waitEndOfTransfer(uint8_t access) {
  while (dma_channel_is_busy(channel[access])) {
    if (inReset()) {
      dma_channel_abort(channel[access]);
      return false;
    }
  }
  while(!pio_sm_is_tx_fifo_empty(pio0, sm[access])) {
    if (inReset()) return false;
  }
  while (pio_sm_get_pc(pio0, sm[access]) != sm_offset[access]) {
    if (inReset()) return false;
  }
  return true;
}

void sendAnswer(uint8_t *buffer, uint32_t nbWord, uint8_t access) {
  restartPio(access);
  startDMA(access, buffer, nbWord);
  pio_sm_set_consecutive_pindirs(pio0, sm[access], CDD0, 8, true);
  setDataDir(DATA_OUT); //Output data
  gpio_put(CDSTEN + access, 0x0);

  waitEndOfTransfer(access);
  gpio_put(CDSTEN + access, 0x1);
  setDataDir(DATA_IN); //input data
  pio_sm_set_consecutive_pindirs(pio0, sm[access], CDD0, 8, false);
  pio_sm_set_enabled(pio0, sm[access], false);
}

static void stopDataTransfer(void) {
  pio_sm_set_enabled(pio0, sm[CHAN_WRITE_DATA], false);
  pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, false);
  gpio_put(CDDTEN, true);
  setDataDir(DATA_IN); //Input data
}

bool sendAnswerStatusMixed(uint8_t *buffer, uint32_t nbWord, uint8_t *buffer_status, uint8_t nbStatus, bool last, bool trace) {
  bool statusStarted = false;
  bool interrupted = false;
  absolute_time_t a = nil_time, b = nil_time, c = nil_time, d = nil_time, e = nil_time, s = get_absolute_time();
  bool lastCDEN = gpio_get(CDEN);
  if (trace) a= get_absolute_time();
  restartPio(CHAN_WRITE_DATA);
  if (trace) b= get_absolute_time();
  startDMA(CHAN_WRITE_DATA, buffer, nbWord);
  if (trace) c= get_absolute_time();
  pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, true);
  if (trace)  d= get_absolute_time();
  setDataDir(DATA_OUT); //Output data
  gpio_put(CDDTEN, 0x0);
  if (trace) e= get_absolute_time();
  absolute_time_t start = get_absolute_time();
  bool canBeInterrupted = false;

  if (trace)
    LOG_SATA("a %lld, b %lld, c %lld, d %lld, e %lld\n", absolute_time_diff_us(s,a), absolute_time_diff_us(a,b), absolute_time_diff_us(b,c), absolute_time_diff_us(c,d), absolute_time_diff_us(d,e));

  while (dma_channel_is_busy(channel[CHAN_WRITE_DATA])) {
    if (inReset()) {
      dma_channel_abort(channel[CHAN_WRITE_DATA]);
      stopDataTransfer();
      return false;
    }
    if (gpio_get(CDEN) != lastCDEN) {
        lastCDEN = !lastCDEN;
        pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, !lastCDEN);
    }
    if((absolute_time_diff_us(start,get_absolute_time()) > 100) && (!statusStarted) && canBeInterrupted && last) {
      statusStarted = true;
      setDataDir(DATA_OUT);
      gpio_put(CDSTEN, 0x0);
    }

    if (!gpio_get(CDEN)) {
      start = get_absolute_time();
      canBeInterrupted = true;
    }

    if (!gpio_get(CDHWR))
    {
      dma_channel_abort(channel[CHAN_WRITE_DATA]);
      stopDataTransfer();
      return false;
    }

    if (gpio_get(CDEN) && statusStarted) {
      interrupted = true;
      pio_sm_set_enabled(pio0, sm[CHAN_WRITE_DATA], false);
      sendAnswer(buffer_status, nbStatus, CHAN_WRITE_STATUS);
      pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, true);
      pio_sm_set_enabled(pio0, sm[CHAN_WRITE_DATA], true);
    }
  }
  while(!pio_sm_is_tx_fifo_empty(pio0, sm[CHAN_WRITE_DATA])) {
    if (inReset()) { stopDataTransfer(); return false; }
  }
  while (pio_sm_get_pc(pio0, sm[CHAN_WRITE_DATA]) != sm_offset[CHAN_WRITE_DATA]) {
    if (inReset()) { stopDataTransfer(); return false; }
  }
  gpio_put(CDDTEN, 0x1);
  setDataDir(DATA_IN); //Input data
  if (!interrupted && last) sendAnswer(buffer_status, nbStatus, CHAN_WRITE_STATUS);
  else pio_sm_set_consecutive_pindirs(pio0, sm[CHAN_WRITE_DATA], CDD0, 8, false);
  pio_sm_set_enabled(pio0, sm[CHAN_WRITE_DATA], false);
  return true;
}

static uint8_t TOC[2048] = {0};
uint8_t FILE_BUFFER[2048] = {0};

static uint32_t read32(const uint8_t *p) {
  return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|((uint32_t)p[3]<<0);
}

static void write32(uint8_t *p, uint32_t v) {
  p[0] = v>>24;
  p[1] = v>>16;
  p[2] = v>>8;
  p[3] = v&0xff;
}

// Append one entry to the TOC buffer. Returns the new length, or -1 if it does not fit
static int appendTocEntry(int toclen, uint32_t flags, uint32_t toc_id, const char *name, uint32_t name_length) {
  if ((toclen < 0) || ((size_t)toclen + 12 + name_length > sizeof(TOC))) return -1;
  write32(&TOC[toclen], flags);
  write32(&TOC[toclen+4], toc_id);
  write32(&TOC[toclen+8], name_length);
  memcpy(&TOC[toclen+12], name, name_length);
  return toclen + 12 + name_length;
}

// Core 0 only (called through the playlist)
char* getPathForTOC(int entry) {
  if (getTocEntry(entry)==0x0) {
    // A whole device: only a CD drive can be played
    device_s *dev = getDevice(((uint32_t)entry)>>24);
    if ((dev == NULL) || !dev->useable || (dev->type != CD_TYPE)) return NULL;
    char* result = malloc(8);
    if (result == NULL) return NULL;
    snprintf(result, 8, "%d://", dev->dev_addr-1);
    return result;
  }
  if (currentImage.curPath == NULL) return NULL;
  size_t i = 0;
  while (i + 12 <= sizeof(TOC)) {
    uint32_t flags = read32(&TOC[i]);
    if (flags == TOC_FLAG_INVALID) return NULL;
    uint32_t toc_id = read32(&TOC[i+4]);
    uint32_t name_length = read32(&TOC[i+8]);
    i += 12;
    if (name_length > sizeof(TOC) - i) return NULL;
    if (toc_id == (uint32_t)entry) {
      if (flags != TOC_FLAG_FILE) return NULL;
      size_t len = strnlen((const char *)&TOC[i], name_length);
      size_t pathLength = strlen(currentImage.curPath) + 1 + len + 1;
      char* result = malloc(pathLength);
      if (result == NULL) return NULL;
      snprintf(result, pathLength, "%s\\%.*s", currentImage.curPath, (int)len, (const char *)&TOC[i]);
      return result;
    }
    i += name_length;
  }
  return NULL;
}

// Core 0 only
void getTocFull(int index, int nb) {
  int toclen = 0;
  int id = 0;
  bool ended = false;
  memset(TOC,0xff,sizeof(TOC));
  if ((getTocLevel() == 0) && (getDeviceCount()>1)) {
    int nbUsb = 0;
    int nbCD = 0;
    int listed = 0;
    for (int i=0; i<CFG_TUH_DEVICE_MAX; i++) {
      if ((nb > 0) && (listed >= nb)) break;
      device_s *dev = getDeviceIndex(i);
      if ((dev->dev_addr == 0xFF) || (dev->type == UNKNOWN_TYPE)) continue;
      char name[20];
      if (dev->type == CD_TYPE) {
        snprintf(name, sizeof(name), "CD %d", nbCD);
        nbCD++;
      } else {
        snprintf(name, sizeof(name), "USB %d", nbUsb);
        nbUsb++;
      }
      uint32_t flags = (dev->isFatFs)?TOC_FLAG_DIR:TOC_FLAG_FILE;
      int newLen = appendTocEntry(toclen, flags, ((uint32_t)dev->dev_addr)<<24, name, strlen(name) + 1);
      if (newLen < 0) break;
      toclen = newLen;
      listed++;
    }
  } else {
    if (!seekTocTo(index)) {
      LOG_SATA("Index %d is out of files number\n", index);
      return;
    }
    while(!ended) {
      toc_entry te;
      memset(&te, 0x0, sizeof(toc_entry));
      if ((index == 0) && (id == 0) && (getTocLevel() != 0)) {
        if (!getReturnTocEntry(&te)) break;
      } else {
        if (!getNextTOCEntry(&te)) break;
      }
      id++;
      if ((nb != -1) && (id >= (nb))) {
        LOG_SATA("Limit has been reached on Id %d (%d)\n", id, nb);
        ended = true;
      }
      te.toc_id &= ~(0xFFu<<24);
      te.toc_id |= ((uint32_t)getTocIndex())<<24;
      int newLen = appendTocEntry(toclen, te.flags, te.toc_id, te.name, te.name_length);
      if (te.name != NULL) free(te.name);
      if (newLen < 0) {
        LOG_SATA("Buffer is full\n");
        break;
      }
      toclen = newLen;
    }
  }
}

// Core 0 only
void getToc(int index, int offset, uint8_t* buffer) {
  if (offset == 0) {
    //init the TOC buffer
    getTocFull(index, -1);
  }
  memset(buffer, 0xFF, 16);
  if ((offset >= 0) && ((size_t)offset < sizeof(TOC))) {
    size_t len = sizeof(TOC) - offset;
    if (len > 16) len = 16;
    memcpy(buffer, &TOC[offset], len);
  }
}

typedef struct {
  int index;
  int offset;
  uint8_t *buffer;
  int entry;
  bool valid;
  bool added;
} toc_job_t;

static void getTocJob(void *arg) {
  toc_job_t *j = (toc_job_t *)arg;
  getToc(j->index, j->offset, j->buffer);
}

static void getTocFullJob(void *arg) {
  toc_job_t *j = (toc_job_t *)arg;
  getTocFull(j->index, j->offset);
}

static void setTocLevelJob(void *arg) {
  toc_job_t *j = (toc_job_t *)arg;
  setTocLevel(j->index);
}

static void addToPlaylistJob(void *arg) {
  toc_job_t *j = (toc_job_t *)arg;
  addToPlaylist(j->entry, &j->valid, &j->added);
}

static void clearPlaylistJob(void *arg) {
  (void) arg;
  clearPlaylist();
}

typedef struct {
  const char *name;
  uint8_t *buf;
  uint16_t length;
  uint16_t done;
  bool write;
  bool success;
} file_job_t;

static void openFileJob(void *arg) {
  file_job_t *j = (file_job_t *)arg;
  j->success = openDataFile(j->name, j->length, j->write);
}

static void readFileJob(void *arg) {
  file_job_t *j = (file_job_t *)arg;
  j->success = readDataFile(j->buf, j->length, &j->done);
}

static void writeFileJob(void *arg) {
  file_job_t *j = (file_job_t *)arg;
  j->success = writeDataFile(j->buf, j->length, &j->done);
}

static void closeFileJob(void *arg) {
  file_job_t *j = (file_job_t *)arg;
  j->success = closeDataFile();
}

// Wait for the USB side. Returns false on error, timeout or 3DO reset.
static bool waitForBlock(void) {
  absolute_time_t deadline = make_timeout_time_ms(READ_TIMEOUT_MS);
  while(!block_is_ready()) {
    if (readError || inReset()) return false;
    if (time_reached(deadline)) {
      LOG_SATA("USB read timeout\n");
      set3doDriveError();
      return false;
    }
  }
  return !readError;
}

absolute_time_t lastPacket;
// Static: a 2449 bytes sector does not fit on the core 1 stack
static uint8_t dataBuffer[2500];

void sendData(int startlba, int nb_block, bool trace) {

  uint8_t status_buffer[2] = {READ_DATA, status};
  absolute_time_t a = nil_time, b = nil_time, c = nil_time, d = nil_time, e = nil_time, s;
  int reste = 0;
  uint32_t size = currentImage.block_size_read;

  if ((nb_block <= 0) || (startlba < 0) || (size == 0) || (size > sizeof(dataBuffer))) {
    if (nb_block > 0) {
      statusSet(CHECK_ERROR);
      errorCode = (startlba < 0) ? ILLEGAL_LBA : ILLEGAL_MODE;
    }
    // Nothing to send: just give the status back
    status_buffer[1] = status;
    sendAnswer(status_buffer, 2, CHAN_WRITE_STATUS);
    return;
  }
  while (nb_block != 0) {
    s = get_absolute_time();
    if (trace) a= get_absolute_time();
    if (!(status & DISC_PRESENT)) {
      return;
    }

    readError = false;
    readBlock(startlba, 1, size, &dataBuffer[0]);
    if (trace) b= get_absolute_time();
    bool ok = waitForBlock();

    if (inReset()) return;
    if (!ok) {
      // Do not send garbage: report the error in the status
      status_buffer[1] = status;
      sendAnswer(status_buffer, 2, CHAN_WRITE_STATUS);
      return;
    }
    if (isAudioBlock(startlba)) {
      int timeForASecond = (990 + pitch) * 1000 + reste;  //Shall be 1s but cd drive is a bit slower than expected
      int correctedDelay = timeForASecond/75;
      reste = timeForASecond % 75;
      if (is_nil_time(lastPacket)) {
        lastPacket = delayed_by_us(get_absolute_time(),correctedDelay);
      } else {
        absolute_time_t currentPacket = get_absolute_time();
        int64_t delay = absolute_time_diff_us(currentPacket, lastPacket); /*Right number shall be 1000000/75*/
        if (delay>0) sleep_us(delay);
        else lastPacket = currentPacket;
        lastPacket = delayed_by_us(lastPacket,correctedDelay);
      }
    }
    if (trace) c = get_absolute_time();
    nb_block--;
    startlba++;
    if (trace) d= get_absolute_time();
    status_buffer[1] = status;
    if (!sendAnswerStatusMixed(&dataBuffer[0], size, status_buffer, 2, nb_block == 0, trace)) return;
    if (trace) e = get_absolute_time();
    if (trace)
      LOG_SATA("send data a %lld, b %lld, c %lld, d %lld, e %lld\n", absolute_time_diff_us(s,a), absolute_time_diff_us(a,b), absolute_time_diff_us(b,c), absolute_time_diff_us(c,d), absolute_time_diff_us(d,e));

  }
}

void sendRawData(int command, uint8_t *buffer, int length) {
  uint8_t status_buffer[2] = {command, status};
  if (length == 0) return;
  if (!sendAnswerStatusMixed(buffer, length, status_buffer, 2, true, false)) return;
}

static volatile bool hasMediaInterrupt = false;
void mediaInterrupt(void) {
  hasMediaInterrupt = true;
}

// Core 0: find what has to be presented to the 3DO after a media change
static void mediaChangeJob(void *arg) {
  (void) arg;
  LOG_SATA("Look for BootImage\n");
  onBootIso = false;
  device_s *current_dev = getDevice(currentImage.dev_addr);
  if ((getPlaylistEntries() == 0) || (current_dev == NULL) || (current_dev->type == CD_TYPE)) {
    MSC_releaseCurrentImage();
  }
  bool loaded = false;
  for (int i=0; i<CFG_TUH_DEVICE_MAX; i++) {
    LOG_SATA("Look for BootImage on %d\n", i);
    device_s *dev = getDeviceIndex(i);
    if (dev->dev_addr == 0xFF) {
      LOG_SATA("%d is not a real device\n", i);
      continue;
    }
    if (dev->type == MSC_TYPE) {
      LOG_SATA("%d is a MSC\n", i);
      // Only one image (boot.iso or playlist entry) per media change
      if (!loaded) loaded = loadBootImage(dev->dev_addr);
      LOG_SATA("Loaded on %d: %d\n", i, loaded);
    }
    if (dev->type == CD_TYPE) {
      LOG_SATA("%d is a CD (%d)\n", i, dev->state);
      wakeUpCDRom(dev->dev_addr, dev->lun);
      set3doCDReady(dev->dev_addr, dev->state == MOUNTED);
      set3doDriveMounted(dev->dev_addr, dev->state  == MOUNTED);
    }
  }
  if (currentImage.dev_addr == 0xFF) {
    // Nothing loaded from a USB key: present the first CD with a disc in it
    for (int i=0; i<CFG_TUH_DEVICE_MAX; i++) {
      device_s *dev = getDeviceIndex(i);
      if ((dev->dev_addr != 0xFF) && (dev->type == CD_TYPE) && (dev->state == MOUNTED) && (dev->rawImage.nb_track > 0)) {
        MSC_releaseCurrentImage();
        memcpy(&currentImage, &dev->rawImage, sizeof(cd_s));
        currentImage.curDir = NULL;
        currentImage.curPath = NULL;
        set3doCDReady(dev->dev_addr, true);
        set3doDriveMounted(dev->dev_addr, true);
        break;
      }
    }
  }
}

void handleMediaInterrupt() {
  if (!hasMediaInterrupt) return;
  if ((status & DISC_PRESENT) && !canHandleReset) {
    gpio_put(LED, ledState);
    ledState = !ledState;
    return;
  }
  hasMediaInterrupt = false;
  gpio_put(CDMDCHG, 0); //Under reset
  critical_section_enter_blocking(&statusLock);
  status &= ~(DISC_PRESENT | SPINNING);
  status |= CHECK_ERROR;
  critical_section_exit(&statusLock);
  errorCode = DISC_REMOVED;
  sleep_ms(10);
  while (!pio_sm_is_rx_fifo_empty(pio0, sm_read)) {
    pio_sm_get(pio0, sm_read);
  }
  runOnCore0(mediaChangeJob, NULL);
  gpio_put(CDMDCHG, 1); //Under reset
}

static const track_s* findTrack(uint8_t id) {
  for (int i = 0; i < currentImage.nb_track; i++) {
    if (currentImage.tracks[i].id == id) return &currentImage.tracks[i];
  }
  return NULL;
}

void handleCommand(uint32_t data) {
  CD_request_t request = (CD_request_t) GET_BUS(data);
  unsigned int data_in[6] = {0};
  uint8_t buffer[20];
  uint8_t subBuffer[16];
  uint index = 0;

  gpio_put(LED, ledState);
  ledState = !ledState;

  // Every command is 7 bytes long: always consume the 6 parameters, even
  // when the command is not answered, so they are not taken as commands.
  if (!get3doParams(data_in)) return;

  switch(request) {
    case READ_ID:
      LOG_SATA("READ ID %x\n", status);
      if (errorOnDisk != 0) errorOnDisk++;
      buffer[index++] = READ_ID;
      buffer[index++] =0x00; //manufacture Id
      buffer[index++] =0x10;
      buffer[index++] =0x00; //manufacture number
      buffer[index++] =0x01;
      buffer[index++] =0x30;
      buffer[index++] =0x75;
      buffer[index++] =0x00; //revision number
      buffer[index++] =0x00;
      buffer[index++] =0x00; //flag
      buffer[index++] =0x00;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case EJECT_DISC:
      LOG_SATA("EJECT\n");
      canHandleReset = true;
      close_tray(false);
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case INJECT_DISC:
      LOG_SATA("INJECT\n");
      close_tray(true);
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case READ_ERROR:
      LOG_SATA("READ ERROR %x %x\n", errorCode, status);
      buffer[index++] = READ_ERROR;
      buffer[index++] = 0x00;
      buffer[index++] = 0x00;
      buffer[index++] = errorCode; //error code
      buffer[index++] = 0x00;
      buffer[index++] = 0x00;
      buffer[index++] = 0x00;
      buffer[index++] = 0x00;
      buffer[index++] = 0x00;
      statusClear(CHECK_ERROR);
      errorCode = NO_ERROR;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case DATA_PATH_CHECK:
      LOG_SATA("DATA_PATH_CHECK %d\n", (status & DISC_PRESENT));
      buffer[index++] = DATA_PATH_CHECK;
      if (!(status & DISC_PRESENT)) {
        buffer[index++] = 0xA5;
        buffer[index++] = 0xA5;
      } else {
        buffer[index++] = 0xAA; //This means ok
        buffer[index++] = 0x55; //This means ok. Not the case when no disc
      }
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      canHandleReset = true;
      break;
    case SPIN_UP:
      if (status & DISC_PRESENT) {
        LOG_SATA("SPIN UP\n");
        buffer[index++] = SPIN_UP;
        if (!(status & TRAY_IN)) {
            statusSet(CHECK_ERROR);
            errorCode = ILLEGAL_CMD;
        } else {
          if (!(status & DISC_RDY)) {
            statusSet(CHECK_ERROR);
            errorCode = DISC_REMOVED;
          } else {
            statusSet(SPINNING);
          }
        }
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      }
      break;
    case READ_DATA:
      if (status & DISC_PRESENT) {
        LOG_SATA("READ DATA %d:%d:%d %x %x\n", data_in[0], data_in[1], data_in[2], data_in[3], status);
        int lba;
        if (data_in[3] == 0x00) {
          //MSF
          lba = data_in[0]*60*75+data_in[1]*75+data_in[2] - 150;
        } else {
          //LBA
          lba = (data_in[0]<<16)|(data_in[1]<<8)|data_in[2];
        }
        int nb_block = (data_in[4]<<8)|data_in[5];
        sendData(lba, nb_block, false);
      }
      break;
    case ABORT:
      LOG_SATA("ABORT\n");
      buffer[index++] = ABORT;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case FLUSH:
        LOG_SATA("FLUSH\n");
        buffer[index++] = FLUSH;
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
        break;
    case READ_DISC_INFO:
      if (status & DISC_PRESENT) {
        LOG_SATA("DISC_INFO\n");
        buffer[index++] = READ_DISC_INFO;
        // LBA = (((M*60)+S)*75+F)-150
        buffer[index++] = currentImage.format;
        buffer[index++] = currentImage.first_track;
        buffer[index++] = currentImage.last_track;
        buffer[index++] = currentImage.msf[0];
        buffer[index++] = currentImage.msf[1];
        buffer[index++] = currentImage.msf[2];
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
        LOG_SATA("%d %x %d %d %d:%d:%d\n", (status & DISC_PRESENT), buffer[1], buffer[2],buffer[3],buffer[4],buffer[5],buffer[6]);
      }
      break;
    case READ_TOC:
      if (status & DISC_PRESENT) {
        LOG_SATA("READ_TOC %x\n", data_in[1]);
        uint8_t ctrl_adr = 0;
        uint8_t id = 0;
        uint8_t msf[3] = {0};
        const track_s *t = findTrack(data_in[1]);
        if (t != NULL) {
          ctrl_adr = t->CTRL_ADR;
          id = t->id;
          memcpy(msf, t->msf, 3);
        } else if ((data_in[1] == 0xAA) && (currentImage.nb_track > 0)) {
          // Lead-out
          ctrl_adr = currentImage.tracks[currentImage.nb_track - 1].CTRL_ADR;
          id = 0xAA;
          memcpy(msf, currentImage.msf, 3);
        } else {
          LOG_SATA("Unknown track %x\n", data_in[1]);
        }
        buffer[index++] = READ_TOC;
        buffer[index++] = 0x0; //NixByte?
        buffer[index++] = ctrl_adr; //ADDR
        buffer[index++] = id; //ENT_NUMBER
        buffer[index++] = 0x0;//Format
        buffer[index++] = msf[0];
        buffer[index++] = msf[1];
        buffer[index++] = msf[2];
        buffer[index++] = 0x0;
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
        LOG_SATA("%x %d %d:%d:%d\n", buffer[2], buffer[3],buffer[5],buffer[6],buffer[7]);
      }
      break;
    case READ_SESSION:
      if (status & DISC_PRESENT) {
        LOG_SATA("READ_SESSION\n");
        buffer[index++] = READ_SESSION;
        if (currentImage.multiSession) {
          //TBD with a multisession disc
          buffer[index++] = 0x80;
          buffer[index++] = currentImage.msf[0]; //might some other values like msf for multisession start
          buffer[index++] = currentImage.msf[1];
          buffer[index++] = currentImage.msf[2];
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
        } else {
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
          buffer[index++] = 0x0;
        }
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
        LOG_SATA("%d:%d:%d\n", buffer[2], buffer[3],buffer[4]);
      }
    break;
    case READ_CAPACITY:
      if (status & DISC_PRESENT) {
        LOG_SATA("READ_CAPACITY\n");
        buffer[index++] = READ_CAPACITY;
        buffer[index++] = currentImage.msf[0];
        buffer[index++] = currentImage.msf[1];
        buffer[index++] = currentImage.msf[2];
        buffer[index++] = 0x0; //0x8?
        buffer[index++] = 0x0;
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      }
      break;
      case SET_MODE:
        LOG_SATA("SET_MODE %x %x %x %x %x %x\n", data_in[0], data_in[1], data_in[2], data_in[3], data_in[4], data_in[5]);
        /*
        not entirely full
[18:04]
cmd9 has many settings
[18:04]
[0] = 0x9
[18:04]
[1] = setting type
[18:04]
where :
[18:04]
0 = density
[18:04]
1 = error recovery
[18:04]
2 = stop time
[18:04]
3 = speed+pitch
[18:05]
4 = chunk size
[18:06]
density : [2] = density code, [3][4] = block length [5] = flags
[18:06]
error recovery : [2] = type [3] = retry count
[18:07]
stop time : [2] = time in 100ms
[18:07]
speed/pitch: [2]=speed [3][4] = pitch
[18:09]
chunk size : [2] = chunk size (1-8)

fixel — Aujourd’hui à 18:17
on density : [5] flags :
[18:18]
0x80 = 2353 bytes CDDA + error correction (modifié)
[18:19]
0x40 = 2448 bytes CDDA+subcode
[18:19]
0xc0 (0x80|0x40) = 2449 bytes : CDDA+subcode+error correction
[18:19]
0x00 : 2048
[18:21]
what's your reply to 0x83?
*/

        if (data_in[0] == 0x3) {
          if (data_in[1] & (0x80)) {
            statusSet(DOUBLE_SPEED);
          } else {
            statusClear(DOUBLE_SPEED);
          }

          if (data_in[2] & 0x4) {
            //Pitch correction On
            pitch = ((data_in[2]&0x3)<<8) | data_in[3];
            if (data_in[2]&0x2) pitch -= 1024;
          } else {
            pitch = 0;
          }
        }
        if (data_in[0] == 0x0) {
          uint32_t size = (data_in[2]<<8)|(data_in[3]);
          if ((data_in[4] & 0xC0) == 0x80) {
            size = 2353; //CDDA + error correction
          }
          if ((data_in[4] & 0xC0) == 0x40) {
            size = 2448; //CDDA+subcode
          }
          if ((data_in[4] & 0xC0) == 0xC0) {
            size = 2449; //CDDA+subcode+error correction
          }
          if ((size == 0) || (size > sizeof(dataBuffer))) {
            // Would not fit in the sector buffer
            LOG_SATA("Unsupported block size %u\n", (unsigned int)size);
            statusSet(CHECK_ERROR);
            errorCode = ILLEGAL_MODE;
          } else {
            currentImage.block_size_read = size;
          }
          LOG_SATA("Block size to %d\n",currentImage.block_size_read );
        }
        buffer[index++] = SET_MODE;
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
        break;
    case READ_SUB_Q:
    if (status & DISC_PRESENT) {
        LOG_SATA("READ_SUB_Q\n");
        buffer[index++] = READ_SUB_Q;
        memset(subBuffer, 0, sizeof(subBuffer));
        readError = false;
        readSubQChannel(&subBuffer[0]);
        if (!waitForBlock()) {
          if (inReset()) return;
          memset(subBuffer, 0, sizeof(subBuffer));
        }

        buffer[index++] = subBuffer[5]; //Ctrl/adr
        buffer[index++] = subBuffer[6]; //trk
        buffer[index++] = subBuffer[7]; //idx
        buffer[index++] = 0;
        buffer[index++] = subBuffer[9]; //total M
        buffer[index++] = subBuffer[10]; //total S
        buffer[index++] = subBuffer[11]; //total F
        buffer[index++] = subBuffer[13]; //current M
        buffer[index++] = subBuffer[14]; //current S
        buffer[index++] = subBuffer[15]; //current F
        buffer[index++] = status;
        sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      }
        break;
//FIXEL Extended commands implementation for ODE menu
    case EXT_ID:
      LOG_SATA("EXT_ID\n");
      buffer[index++] = EXT_ID;
      buffer[index++] = 0x1; //Where is located the boot.iso microsd=1, usbmsc=2, flash=4, sata=8
      buffer[index++] = 'L';
      buffer[index++] = 'O';
      buffer[index++] = 'C';
      buffer[index++] = 'O';
      buffer[index++] = 'D';
      buffer[index++] = 'E';
      buffer[index++] = 1; //rev major
      buffer[index++] = 1; //rev minor
      buffer[index++] = 0; //rev patch
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case CHANGE_TOC:
      buffer[index++] = CHANGE_TOC;
      {
        toc_job_t job = {0};
        job.index = (int)(((data_in[0]&0xFF)<<24)|((data_in[1]&0xFF)<<16)|((data_in[2]&0xFF)<<8)|((data_in[3]&0xFF)<<0));
        LOG_SATA("CHANGE_TOC %d on %u\n", job.index&0xFFFFFF, ((unsigned int)job.index)>>24);
        runOnCore0(setTocLevelJob, &job);
      }
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case GET_TOC:
      buffer[index++] = GET_TOC;
      {
        toc_job_t job = {0};
        job.index = (int)(((data_in[0]&0xFF)<<24)|((data_in[1]&0xFF)<<16)|((data_in[2]&0xFF)<<8)|((data_in[3]&0xFF)<<0));
        job.offset = ((data_in[4]&0xFF)<<8)|((data_in[5]&0xFF)<<0);
        job.buffer = &buffer[index];
        LOG_SATA("GET_TOC %d %d\n", job.index, job.offset);
        runOnCore0(getTocJob, &job);
        for (int i=0; i<16; i++) LOG_SATA("%x ", buffer[index+i]);
        LOG_SATA("\n");
        index += 16;
      }
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case GET_DESC:
      LOG_SATA("GET_DESC\n");
      buffer[index++] = GET_DESC;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case CLEAR_PLAYLIST:
      LOG_SATA("CLEAR_PLAYLIST\n");
      buffer[index++] = CLEAR_PLAYLIST;
      buffer[index++] = status;
      runOnCore0(clearPlaylistJob, NULL);
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case ADD_PLAYLIST:
      buffer[index++] = ADD_PLAYLIST;
      {
        toc_job_t job = {0};
        job.entry = (int)(((data_in[0]&0xFF)<<24)|((data_in[1]&0xFF)<<16)|((data_in[2]&0xFF)<<8)|((data_in[3]&0xFF)<<0));
        LOG_SATA("ADD_PLAYLIST %d\n", job.entry);
        runOnCore0(addToPlaylistJob, &job);
        buffer[index++] = job.valid;
        buffer[index++] = job.added;
      }
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case LAUNCH_PLAYLIST:
      LOG_SATA("LAUNCH_PLAYLIST\n");
      //Initiate an media change error
      statusSet(CHECK_ERROR);
      buffer[index++] = LAUNCH_PLAYLIST;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      mediaInterrupt();
      break;
    case GET_TOC_LIST:
      {
        toc_job_t job = {0};
        job.index = (int)(((data_in[0]&0xFF)<<24)|((data_in[1]&0xFF)<<16)|((data_in[2]&0xFF)<<8)|((data_in[3]&0xFF)<<0));
        job.offset = ((data_in[4]&0xFF)<<8)|((data_in[5]&0xFF)<<0);
        LOG_SATA("GET_TOC_LIST %d %d\n", job.index, job.offset);
        runOnCore0(getTocFullJob, &job);
      }
      sendRawData(GET_TOC_LIST, &TOC[0], 2048);
#ifdef USE_TRACE
      LOG_SATA("TOC Buffer:\n");
      for (size_t i=0; i + 12 <= sizeof(TOC);) {
        uint32_t flags = read32(&TOC[i]);
        if (flags == TOC_FLAG_INVALID) break;
        uint32_t toc_id = read32(&TOC[i+4]);
        uint32_t name_length = read32(&TOC[i+8]);
        i += 12;
        if (name_length > sizeof(TOC) - i) break;
        LOG_SATA(".flags :0x%lx, .toc_id: %lu %d, .name_length: %lu, .name: %.*s\n", flags, toc_id>>24, getTocEntry(toc_id), name_length, (int)name_length, &TOC[i]);
        i += name_length;
      }
      LOG_SATA("\n");
#endif
      break;
    //Not supported yet
    case CREATE_FILE:
      LOG_SATA("CREATE_FILE\n");
      buffer[index++] = CREATE_FILE;
      buffer[index++] = 0; //Always report a failure
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case OPEN_FILE:
      {
        file_job_t job = {0};
        job.write = (data_in[5]!=0);
        if ((data_in[0]&0xFF) != 0) {
          unsigned int toc_id = ((data_in[1]&0xFF)<<24)|((data_in[2]&0xFF)<<16)|((data_in[3]&0xFF)<<8)|((data_in[4]&0xFF)<<0);
          LOG_SATA("OPEN_FILE TOC_ID %x, W/R %d (not supported)\n", toc_id, job.write);
        } else {
          uint16_t name_length = ((data_in[1]&0xFF)<<8)|((data_in[2]&0xFF)<<0);
          if (name_length > sizeof(FILE_BUFFER)) name_length = sizeof(FILE_BUFFER);
          LOG_SATA("OPEN_FILE NAME %d %.*s, W/R %d\n", name_length, name_length, &FILE_BUFFER[0], job.write);
          job.name = (const char *)&FILE_BUFFER[0];
          job.length = name_length;
          runOnCore0(openFileJob, &job);
        }
        buffer[index++] = OPEN_FILE;
        buffer[index++] = job.success;
        buffer[index++] = status;
      }
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case SEEK_FILE:
      LOG_SATA("SEEK_FILE\n");
      buffer[index++] = SEEK_FILE;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case READ_FILE_BYTE:
    case WRITE_FILE_BYTE:
      {
        file_job_t job = {0};
        uint16_t length = ((data_in[0]&0xFF)<<8)|((data_in[1]&0xFF)<<0);
        // FILE_BUFFER is the only exchange buffer: never go beyond it
        if (length > sizeof(FILE_BUFFER)) length = sizeof(FILE_BUFFER);
        LOG_SATA("%s %x\n", (request == READ_FILE_BYTE)?"READ_FILE_BYTE":"WRITE_FILE_BYTE", length);
        job.buf = &FILE_BUFFER[0];
        job.length = length;
        runOnCore0((request == READ_FILE_BYTE) ? readFileJob : writeFileJob, &job);
        // Report the number of bytes really transferred
        buffer[index++] = request;
        buffer[index++] = job.success ? (job.done>>8) : 0;
        buffer[index++] = job.success ? (job.done&0xFF) : 0;
      }
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case CLOSE_FILE:
      {
        file_job_t job = {0};
        LOG_SATA("CLOSE_FILE\n");
        runOnCore0(closeFileJob, &job);
      }
      buffer[index++] = CLOSE_FILE;
      buffer[index++] = status;
      sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case WRITE_BUFFER_OFFSET:
    {
      unsigned int offset = (((data_in[0]&0xFF)<<8)|((data_in[1]&0xFF)<<0))<<2;
      LOG_SATA("WRITE_BUFFER_OFFSET 0x%x\n", offset);
      if (offset + 4 <= sizeof(FILE_BUFFER)) {
        for (int i=0; i<4; i++) {
          FILE_BUFFER[offset+i] = data_in[2+i];
        }
      } else {
        LOG_SATA("Offset out of buffer\n");
      }
    }
    buffer[index++] = WRITE_BUFFER_OFFSET;
    buffer[index++] = status;
    sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    case READ_BUFFER:
    LOG_SATA("READ_BUFFER\n");
    sendRawData(READ_BUFFER, &FILE_BUFFER[0], 2048);
      break;
    case UPDATE_ODE:
    LOG_SATA("UPDATE_ODE\n");
    buffer[index++] = UPDATE_ODE;
    buffer[index++] = status;
    sendAnswer(buffer, index, CHAN_WRITE_STATUS);
      break;
    default: LOG_SATA("unknown Cmd %x\n", request);
  }
}

void setupDMA(uint8_t access) {
  channel[access] = dma_claim_unused_channel(true);
  config[access] = dma_channel_get_default_config(channel[access]);
  channel_config_set_transfer_data_size(&config[access], DMA_SIZE_8);
  channel_config_set_read_increment(&config[access], true);
  channel_config_set_write_increment(&config[access], false);
  channel_config_set_irq_quiet(&config[access], true);
  channel_config_set_dreq(&config[access], pio_get_dreq(pio0, sm[access], true));
  dma_channel_set_write_addr(channel[access], &pio0->txf[sm[access]], false);
  dma_channel_set_config(channel[access], &config[access], false);

  bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
}

void pio_program_init(int channel) {
  sm[channel] = channel;
  sm_offset[channel] = pio_add_program(pio0, &write_program);
  write_program_init(pio0, sm[channel], sm_offset[channel]);
  instr_jmp[channel] = pio_encode_jmp(sm_offset[channel]);
  LOG_SATA("Prog %d offset %d\n", channel, sm_offset[channel]);
}

static absolute_time_t s;
static bool debounceEject = false;

void core1_entry() {

  uint32_t data_in;
  bool ejectState = gpio_get(EJECT);

  lastPacket = nil_time;

  instr_out = pio_encode_out(pio_null, 16);
  instr_pull = pio_encode_pull(pio_null, 32);
  instr_set = pio_encode_set(pio_pins, 0b11);

  for (int i = 0; i< CHAN_MAX; i++) {
    pio_program_init(i);
    setupDMA(i);
  }

  LOG_SATA("Ready\n");
  while (1){

    if (use_cdrom) {
      gpio_put(CDRST_SNIFF, gpio_get(CDRST));
      gpio_put(CDEN_SNIFF, gpio_get(CDEN));
    } else {
      if (!gpio_get(CDRST)) {
        LOG_SATA("Got a reset Status!\n");
        wait_out_of_reset();
        statusSet(CHECK_ERROR);
        errorCode = POWER_OR_RESET_OCCURED;
        restartReadPio();
      }
      if (gpio_get(CDEN)) {
        LOG_SATA("CD is not enabled\n");
        while(gpio_get(CDEN) && gpio_get(CDRST));
        LOG_SATA("CD is enabled now\n");
      }

      bool ejectCurrent = gpio_get(EJECT);
      if ((ejectCurrent != ejectState)) {
        if (!debounceEject) s = get_absolute_time();
        debounceEject = true;
        if (absolute_time_diff_us(s, get_absolute_time()) > 25000) {
          ejectState = ejectCurrent;
          //Eject button pressed, toggle tray position
          if (!ejectCurrent) {
            close_tray((status & TRAY_IN) == 0);
          }
          debounceEject = false;
        }
      } else {
        if (debounceEject && (absolute_time_diff_us(s, get_absolute_time()) > 25000)) debounceEject = false;
      }

      if (is3doData()) {
        data_in = get3doData();
        handleCommand(data_in);
      }
      handleMediaInterrupt();
    }
  }
}

void _3DO_init() {
  // use_cdrom = true;
  uint offset = -1;

  critical_section_init(&statusLock);

  gpio_init(CDRST);
  gpio_set_dir(CDRST, false);

  gpio_init(EJECT);
  gpio_set_dir(EJECT, false);

  gpio_init(CDWAIT);
  gpio_put(CDWAIT, 1);  //CDWAIT is always 1
  gpio_set_dir(CDWAIT, (!use_cdrom));

  gpio_init(CDEN);
  gpio_set_dir(CDEN, false);

  gpio_init(CDMDCHG);
  gpio_put(CDMDCHG, 1);
  gpio_set_dir(CDMDCHG, (!use_cdrom));


  gpio_init(CDSTEN);
  gpio_put(CDSTEN, 1);
  gpio_set_dir(CDSTEN, (!use_cdrom));

  gpio_init(CDDTEN);
  gpio_put(CDDTEN, 1);
  gpio_set_dir(CDDTEN, (!use_cdrom));

  gpio_init(LED);
  gpio_set_dir(LED, (!use_cdrom));

  gpio_init(CDHRD);
  gpio_set_dir(CDHRD, false);

  gpio_init(CDHWR);
  gpio_set_dir(CDHWR, false);

  gpio_init(DIR_DATA);
  gpio_put(DIR_DATA, DATA_IN); //Input data
  gpio_set_dir(DIR_DATA,  (!use_cdrom));

  gpio_set_dir_masked(DATA_BUS_MASK, DATA_BUS_MASK);
  gpio_init_mask(DATA_BUS_MASK);

  gpio_init(CDRST_SNIFF);
  gpio_put(CDRST_SNIFF, 0);
  gpio_set_dir(CDRST_SNIFF, (!use_cdrom));

  gpio_init(CDEN_SNIFF);
  gpio_put(CDEN_SNIFF, 1);
  gpio_set_dir(CDEN_SNIFF, (!use_cdrom));

  if (!use_cdrom) {

    sm_read = CHAN_MAX;
    offset = pio_add_program(pio0, &read_program);
    read_program_init(pio0, sm_read, offset);
    instr_jmp_read = pio_encode_jmp(offset);


    for (int i = 0; i<30; i++) {
       gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
    }

    pio_gpio_init(pio0, CDD0);
    pio_gpio_init(pio0, CDD1);
    pio_gpio_init(pio0, CDD2);
    pio_gpio_init(pio0, CDD3);
    pio_gpio_init(pio0, CDD4);
    pio_gpio_init(pio0, CDD5);
    pio_gpio_init(pio0, CDD6);
    pio_gpio_init(pio0, CDD7);
  }


  LOG_SATA("wait for msg now\n");


  int clock = clock_get_hz(clk_sys);
  LOG_SATA("Clock is %d\n", clock);

  multicore_launch_core1(core1_entry);
}
