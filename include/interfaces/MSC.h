#ifndef __MSC_H_INCLUDE__
#define __MSC_H_INCLUDE__

#include "USB.h"
#include "TOC.h"

extern bool onBootIso;

extern bool MSC_Host_loop(device_s *dev);

extern bool MSC_Inquiry(uint8_t dev_addr, uint8_t lun);

extern void setTocLevel(int index);
extern int getTocLevel(void);
extern int getTocIndex(void);
extern int getTocEntry(int index);

extern bool getNextTOCEntry(toc_entry* toc);
extern bool getReturnTocEntry(toc_entry* toc);
extern void getToc(int index, int offset, uint8_t* buffer);
extern void getTocFull(int index, int nb);

extern char* getPathForTOC(int entry);

extern void printPlaylist();
extern void clearPlaylist(void);
extern void addToPlaylist(int entry, bool *valid, bool *added);

extern bool seekTocTo(int index);

// File services for the ODE menu. They must be executed on core 0.
extern bool openDataFile(const char* name, uint16_t name_length, bool write);
extern bool writeDataFile(const uint8_t* buf, uint16_t length, uint16_t *written);
extern bool readDataFile(uint8_t* buf, uint16_t length, uint16_t *read);
extern bool closeDataFile(void);

// Load boot.iso or the next playlist entry from this device. Core 0 only.
extern bool loadBootImage(uint8_t dev_addr);

// Forget the current image (menu context and image files). Core 0 only.
extern void MSC_releaseCurrentImage(void);

extern int getPlaylistEntries();
extern void updatePlayListEntries();

#endif
