#include <stdio.h>
#include "USB.h"
#include "CDFormat.h"
#include "3DO.h"
#include "CDROM.h"
#include "MSC.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"

static int deviceCnt = 0;

volatile bool usb_cmd_on_going = false;

static scsi_inquiry_resp_t inquiry_resp;
static bool inquiry_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw);

static device_s devices[CFG_TUH_DEVICE_MAX] = {0};

static volatile bool usbResetRequested = false;

//--------------------------------------------------------------------+
// Jobs executed on core 0 on behalf of core 1
//--------------------------------------------------------------------+
static volatile core0_job_t core0_job = NULL;
static void * volatile core0_job_arg = NULL;
static volatile bool core0_job_done = false;

void runOnCore0(core0_job_t job, void *arg) {
  if (job == NULL) return;
  if (get_core_num() == 0) {
    job(arg);
    return;
  }
  core0_job_arg = arg;
  core0_job_done = false;
  __dmb();
  core0_job = job;
  while (!core0_job_done) tight_loop_contents();
  __dmb();
}

static void processCore0Job(void) {
  core0_job_t job = core0_job;
  if (job == NULL) return;
  __dmb();
  job(core0_job_arg);
  core0_job = NULL;
  __dmb();
  core0_job_done = true;
}

//--------------------------------------------------------------------+
// Device list
//--------------------------------------------------------------------+
device_s* getDevice(uint8_t dev_addr) {
  if (dev_addr == 0xFF) return NULL;
  for (int i = 0; i<CFG_TUH_DEVICE_MAX; i++) {
    if (devices[i].dev_addr == dev_addr)
      return &devices[i];
  }
  return NULL;
}

static device_s* allocDevice(uint8_t dev_addr) {
  device_s *dev = getDevice(dev_addr);
  if (dev != NULL) return dev;
  for (int i = 0; i<CFG_TUH_DEVICE_MAX; i++) {
    if (devices[i].dev_addr == 0xFF) {
      devices[i].dev_addr = dev_addr;
      return &devices[i];
    }
  }
  return NULL;
}

device_s* getDeviceIndex(uint8_t idx) {
  if(idx >= CFG_TUH_DEVICE_MAX) return NULL;
  else return &devices[idx];
}

static void resetDeviceEntry(device_s *dev) {
  dev->dev_addr = 0xFF;
  dev->useable = false;
  dev->canBeLoaded = false;
  dev->canBeEjected = false;
  dev->isFatFs = false;
  dev->state = DETACHED;
  dev->type = UNKNOWN_TYPE;
  dev->tray_open = false;
  dev->counted = false;
  dev->pendingMount = false;
  dev->mediaCheckPending = false;
}

void USB_Host_init() {
    for (int i = 0; i<CFG_TUH_DEVICE_MAX; i++) {
      resetDeviceEntry(&devices[i]);
    }
    currentImage.curDir = NULL;
    currentImage.curPath = NULL;
    currentImage.dev_addr = 0xFF;
    currentImage.lun = 0x0;
    usb_cmd_on_going = false;
    deviceCnt = 0;
#ifdef USE_TRACE
    stdio_init_all();
#endif
    tusb_init();
}

static bool check_eject(device_s *dev);

static bool Default_Host_loop(device_s *dev) {
#if CFG_TUH_MSC
  if (!usb_cmd_on_going) {
    switch(dev->state) {
      case EJECTING:
        check_eject(dev);
        return true;
      case MOUNTED:
        return true;
      default:
        return false;
    }
  }
#endif
  return false;
}

void USB_Host_loop()
{
  // tinyusb host task
  bool ret = true;
  tuh_task();

  if (usbResetRequested) {
    usbResetRequested = false;
    USB_reset();
    return;
  }

  // Work requested by the 3DO core
  processCore0Job();

  for (int i =0; i<CFG_TUH_DEVICE_MAX; i++) {
    device_s *dev = getDeviceIndex(i);
    if (dev->dev_addr == 0xFF) continue;

    if (dev->pendingMount) {
      // Done here and not in the mount callback: mounting reads the drive,
      // which runs tuh_task() and must not be done from inside TinyUSB.
      dev->pendingMount = false;
      if (dev->type == MSC_TYPE) MSC_Inquiry(dev->dev_addr, dev->lun);
    }

    if (dev->mediaCheckPending && time_reached(dev->mediaCheckAt)) {
      dev->mediaCheckPending = false;
      checkForMedia(dev->dev_addr, dev->lun);
    }

    switch (dev->type) {
      case CD_TYPE:
        ret = CDROM_Host_loop(dev);
        break;
      case MSC_TYPE:
        ret = MSC_Host_loop(dev);
        break;
      default:
        ret = Default_Host_loop(dev);
      }
      if (!ret) {
        if(dev->state==INQUIRY) {
          dev->state = ATTACHED;
          tuh_msc_inquiry(dev->dev_addr, 0, &inquiry_resp, inquiry_complete_cb);
        }
      }
  }
}


#if CFG_TUH_MSC

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+

cd_s currentImage = {0};

volatile bool read_done;
volatile bool is_audio;
volatile bool has_subQ;

volatile uint32_t start_Block;
volatile uint32_t nb_block_Block;
uint8_t * volatile buffer_Block;
volatile bool blockRequired = false;
volatile bool subqRequired = false;
uint8_t * volatile buffer_subq;

static bool check_eject(device_s *dev) {
  //Execute right now
  LOG_SATA("Eject\n");
  if (CDROM_ExecuteEject(dev->dev_addr)) dev->state = INQUIRY;
  return true;
}

// Can be called from any context: the reset is executed later by USB_Host_loop
void USB_requestReset(void) {
  usbResetRequested = true;
}

void USB_reset() {
  LOG_SATA("reset usb\n");
  for (int i=0; i<CFG_TUH_DEVICE_MAX; i++) {
    device_s *dev = getDeviceIndex(i);
    if (dev->dev_addr != 0xFF) tuh_msc_umount_cb(dev->dev_addr);
  }
  usb_cmd_on_going = false;
  tusb_reset();
}

static bool inquiry_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw)
{
  device_s *dev = getDevice(dev_addr);
  if (dev == NULL) return false;
  if (csw->status != MSC_CSW_STATUS_GOOD)
  {
    TU_LOG1("Inquiry failed %x\r\n", csw->status);
    dev->state = INQUIRY;
    return false;
  }
  //At least consider we are enumerated so that configuration can be read
  if (dev->state < ENUMERATED) dev->state = ENUMERATED;
  // Print out Vendor ID, Product ID and Rev
  TU_LOG1("%.8s %.16s rev %.4s Type 0x%x Lun %d\r\n", inquiry_resp.vendor_id, inquiry_resp.product_id, inquiry_resp.product_rev, inquiry_resp.peripheral_device_type, cbw->lun);

  dev->lun = cbw->lun;

  if (inquiry_resp.peripheral_device_type == 0x5) {
    dev->type = CD_TYPE;
  }
  if (inquiry_resp.peripheral_device_type == 0x0) {
    dev->type = MSC_TYPE;
  }
  // The inquiry is done again after each eject: count the device only once
  if ((dev->type != UNKNOWN_TYPE) && !dev->counted) {
    dev->counted = true;
    deviceCnt++;
  }
  checkForMedia(dev_addr, dev->lun);

  return true;
}

void tuh_msc_ready_cb(uint8_t dev_addr, bool ready) {
  device_s *dev=getDevice(dev_addr);
  if (dev == NULL) return;
  if (dev->type == CD_TYPE)
    CDROM_ready(dev_addr, ready);
  if (!ready) {
    // Check again later. Not done from a timer interrupt because TinyUSB
    // is not interrupt safe: USB_Host_loop sends the command when it is time.
    dev->mediaCheckAt = make_timeout_time_ms(500);
    dev->mediaCheckPending = true;
  }
}

void tuh_msc_enumerate_cb (uint8_t dev_addr) {
  device_s *dev = allocDevice(dev_addr);
  TU_LOG1("Usb device enumerated %x\n", dev_addr);
  if (dev == NULL) {
    LOG_SATA("No room for device %d\n", dev_addr);
    return;
  }
  dev->state = INQUIRY;
}

bool read_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw) {
  (void) dev_addr; (void) cbw;
  if (csw->status != MSC_CSW_STATUS_GOOD) {
    set3doDriveError();
  }
  read_done = true;
  usb_cmd_on_going = false;
  return true;
}

bool block_is_ready() {
  return read_done;
}

bool USBDriveEject(uint8_t dev_addr, bool eject) {
  (void) eject;
  device_s *dev = getDevice(dev_addr);
  if (dev == NULL) return false;
  if (dev->type == CD_TYPE) {
    dev->state = EJECTING;
    return true;
  }
  if (dev->type == MSC_TYPE) {
    return true;
  }
  return false;
}

int getDeviceCount(void) {
  return deviceCnt;
}


bool readSubQChannel(uint8_t *buffer) {
  buffer_subq = buffer;
  read_done = false;
  __dmb();
  subqRequired = true;
  return true;
}

static int trackForBlock(uint32_t start) {
  int res = -1;
  for (int i = 0; i < currentImage.nb_track; i++) {
    if (currentImage.tracks[i].start_lba <= (int32_t)start) res = i;
  }
  return res;
}

bool isAudioBlock(uint32_t start) {
  int t = trackForBlock(start);
  if (t < 0) return false;
  return ((currentImage.tracks[t].CTRL_ADR & 0x4) == 0x0);
}

bool readBlock(uint32_t start, uint16_t nb_block, uint16_t block_size, uint8_t *buffer) {
  int t = trackForBlock(start);
  read_done = false;
  start_Block = start;
  nb_block_Block = nb_block;
  buffer_Block = buffer;
  is_audio = (t >= 0) && ((currentImage.tracks[t].CTRL_ADR & 0x4) == 0x0);
  has_subQ = (t >= 0) && (block_size >= 2448);
  __dmb();
  blockRequired = true;
  return true;
}
//------------- IMPLEMENTATION -------------//

void tuh_msc_enumerated_cb(uint8_t dev_addr)
{
  (void) dev_addr;
  TU_LOG1("##### enumerated ######\n");
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
  device_s *dev = allocDevice(dev_addr);
  LOG_SATA("A USB MassStorage device is mounted %d\r\n", dev_addr);
  if (dev == NULL) return;
  dev->state = MOUNTED;

  if (dev->type == CD_TYPE) {
    // Only queues SCSI commands, safe from the callback
    CDROM_Inquiry(dev_addr, dev->lun);
  }
  if (dev->type == MSC_TYPE) {
    // Mounting the FAT reads the drive: deferred to USB_Host_loop
    dev->pendingMount = true;
  }
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
  device_s *dev = getDevice(dev_addr);

  LOG_SATA("A USB MassStorage device is unmounted\r\n");
  if (dev == NULL) return;

  set3doCDReady(dev_addr, false);
  // Releases currentImage if it was using this device
  set3doDriveMounted(dev_addr, false);

  if (dev->isFatFs) {
    char path[8];
    snprintf(path, sizeof(path), "%d:", dev_addr-1);
    f_mount(NULL, path, 0);
  }
  if (dev->counted) deviceCnt--;
  resetDeviceEntry(dev);
  mediaInterrupt();
}

#endif
