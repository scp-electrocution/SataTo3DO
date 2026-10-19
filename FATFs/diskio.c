/*-----------------------------------------------------------------------*/
/* Low level disk I/O module skeleton for FatFs     (C)ChaN, 2007        */
/*-----------------------------------------------------------------------*/
/* This is a stub disk I/O module that acts as front end of the existing */
/* disk I/O modules and attach it to FatFs module with common interface. */
/*-----------------------------------------------------------------------*/
#include "ffconf.h"
#include "diskio.h"

/* FatFs physical drive N is the USB device with address N+1 */
#define DRV_COUNT ((CFG_TUH_DEVICE_MAX < FF_VOLUMES) ? CFG_TUH_DEVICE_MAX : FF_VOLUMES)

static DSTATUS disk_state[DRV_COUNT];

void diskio_init(void)
{
	memset(disk_state, STA_NOINIT, sizeof(disk_state));
}

/*-----------------------------------------------------------------------*/
/* Initialize a Drive                                                    */

DSTATUS disk_initialize (
	BYTE drv				/* Physical drive nmuber (0..) */
)
{
	if (drv >= DRV_COUNT) return STA_NOINIT;
	disk_state[drv] &= (~STA_NOINIT); // clear NOINIT bit
	return disk_state[drv];
}

void disk_deinitialize ( BYTE drv )
{
	if (drv >= DRV_COUNT) return;
	disk_state[drv] |= STA_NOINIT; // set NOINIT bit
}

/*-----------------------------------------------------------------------*/
/* Return Disk Status                                                    */

DSTATUS disk_status (
	BYTE drv		/* Physical drive nmuber (0..) */
)
{
	if (drv >= DRV_COUNT) return STA_NOINIT;
	return disk_state[drv];
}

/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */

static volatile bool read_flag;
static volatile bool read_ok;
static volatile bool write_flag;
static volatile bool write_ok;

static bool disk_read10_cb(uint8_t dev_addr, const msc_cbw_t *cbw, const msc_csw_t *csw) {
	(void) dev_addr; (void) cbw;
	read_ok = (csw->status == MSC_CSW_STATUS_GOOD);
	read_flag = true;
	return true;
}

static bool disk_write10_cb(uint8_t dev_addr, const msc_cbw_t *cbw, const msc_csw_t *csw) {
	(void) dev_addr; (void) cbw;
	write_ok = (csw->status == MSC_CSW_STATUS_GOOD);
	write_flag = true;
	return true;
}

DRESULT disk_read (
	BYTE drv,		/* Physical drive nmuber (0..) */
	BYTE *buff,		/* Data buffer to store read data */
	DWORD sector,	/* Sector address (LBA) */
	UINT count		/* Number of sectors to read */
)
{
	if ((drv >= DRV_COUNT) || (count == 0) || (count > 0xFFFF)) return RES_PARERR;
	uint8_t usb_addr = drv+1;
	read_flag = false;
	read_ok = false;
	if ( !tuh_msc_read10(usb_addr, 0, buff, sector, (uint16_t)count, disk_read10_cb)) {
		return RES_ERROR;
	}
	uint64_t time_limit = time_us_64() + TIMEOUT_WAIT_US;
	while (!read_flag) {
		tuh_task();
		if (time_us_64() > time_limit) {
			/* The transfer can not be cancelled with this USB stack: the device is
			   most probably gone, report the error to FatFs. */
			return RES_ERROR;
		}
	}
	return read_ok ? RES_OK : RES_ERROR;
}

/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */

#if _READONLY == 0
DRESULT disk_write (
	BYTE drv,			/* Physical drive nmuber (0..) */
	const BYTE *buff,	/* Data to be written */
	DWORD sector,		/* Sector address (LBA) */
	UINT count			/* Number of sectors to write */
)
{
	if ((drv >= DRV_COUNT) || (count == 0) || (count > 0xFFFF)) return RES_PARERR;
	uint8_t usb_addr = drv+1;
	write_flag = false;
	write_ok = false;
	if ( !tuh_msc_write10(usb_addr, 0, buff, sector, (uint16_t)count, disk_write10_cb) ) {
		return RES_ERROR;
	}
	uint64_t time_limit = time_us_64() + TIMEOUT_WAIT_US;
	while (!write_flag) {
		tuh_task();
		if (time_us_64() > time_limit) {
			return RES_ERROR;
		}
	}
	return write_ok ? RES_OK : RES_ERROR;
}
#endif /* _READONLY */

/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */

DRESULT disk_ioctl (
	BYTE drv,		/* Physical drive nmuber (0..) */
	BYTE ctrl,		/* Control code */
	void *buff		/* Buffer to send/receive control data */
)
{
	(void) drv; (void) buff;
	if (ctrl == CTRL_SYNC)
		return RES_OK;
	else
		return RES_PARERR;
}

DWORD get_fattime (void)
{
	return ((DWORD)(20 + 1) << 25) |
	              ((DWORD)1 << 21) |
	              ((DWORD)1 << 16) |
	              ((DWORD)1 << 11) |
	              ((DWORD)1 << 5)  |
	       (((DWORD)1 >> 1) << 0);
}
