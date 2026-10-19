#ifndef __USB_HOST_H_INCLUDE__
#define __USB_HOST_H_INCLUDE__

#include "tusb.h"
#include "diskio.h"
#include "CDFormat.h"
#include "pico/time.h"


typedef enum {
  DETACHED = 0x00,
  INQUIRY,
  ATTACHED,
  ENUMERATED,
  CONFIGURED,
  MOUNTED,
  EJECTING,
} usb_state_t;

typedef enum {
  UNKNOWN_TYPE = 0x00,
  MSC_TYPE = 0x1,
  CD_TYPE = 0x2,
} device_type_t;

typedef struct {
  // bool mounted;
  uint8_t dev_addr;
  uint8_t lun;
  bool canBeEjected;
  bool canBeLoaded;
  bool isFatFs;
  bool useable;
  device_type_t type;
  usb_state_t state;
  bool tray_open;
  bool counted;            // device is accounted in the device counter
  bool pendingMount;       // mount handling deferred out of the TinyUSB callback
  bool mediaCheckPending;  // a "test unit ready" has to be sent at mediaCheckAt
  absolute_time_t mediaCheckAt;
  FATFS  DiskFATState;
  cd_s rawImage;
} device_s;

extern volatile bool usb_cmd_on_going;

// Shared between the 3DO core (core 1) and the USB core (core 0)
extern volatile bool read_done;
extern volatile bool is_audio;
extern volatile bool has_subQ;
extern volatile uint32_t start_Block;
extern volatile uint32_t nb_block_Block;
extern uint8_t * volatile buffer_Block;
extern volatile bool blockRequired;
extern volatile bool subqRequired;
extern uint8_t * volatile buffer_subq;

extern void USB_Host_init();
extern void USB_Host_loop();
extern void USB_reset(void);
extern void USB_requestReset(void);

// Lookup only: returns NULL if no device with this address is known
extern device_s* getDevice(uint8_t dev_addr);
extern device_s* getDeviceIndex(uint8_t idx);
extern int getDeviceCount(void);

extern bool read_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw);

extern bool readBlock(uint32_t start, uint16_t nb_block, uint16_t block_size, uint8_t *buffer);
extern bool readSubQChannel(uint8_t *buffer);
extern bool block_is_ready(void);
extern bool isAudioBlock(uint32_t start);
extern bool USBDriveEject(uint8_t dev_addr, bool eject);

// Execute a job on core 0 (the core that owns TinyUSB and FatFs).
// When called from core 1, it blocks until the job has been executed.
typedef void (*core0_job_t)(void *arg);
extern void runOnCore0(core0_job_t job, void *arg);

#endif
