//
//  HyperVBalloon.hpp
//  Hyper-V balloon driver interface
//
//  Copyright © 2022-2026 xdqi. All rights reserved.
//

#ifndef HyperVBalloon_hpp
#define HyperVBalloon_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

#include "HyperVVMBusDevice.hpp"
#include "HyperVBalloonRegs.hpp"
#include "HyperVBalloonPageIndex.hpp"

//
// Ballooned memory allocation, a physically contiguous run of pages reported to the host as one page range.
//
typedef struct {
  UInt64                    basePageFrameNumber;
  UInt32                    pageCount;
  UInt32                    returnedPageCount;
  IOBufferMemoryDescriptor  *memoryDescriptor;
  // Pages not yet returned by the host are hidden from the kernel memory statistics
  bool                      isHidden;
  // Pages already returned by the host
  UInt64                    returnedPages[kHyperVDynamicMemoryChunkPageCount / 64];
} HyperVBalloonChunk;

class HyperVBalloon : public IOService {
  OSDeclareDefaultStructors(HyperVBalloon);
  HVDeclareLogFunctionsVMBusChild("ball");
  typedef IOService super;

private:
  HyperVVMBusDevice  *_hvDevice                             = nullptr;

  IOWorkLoop         *_workLoop                             = nullptr;
  IOTimerEventSource *_timerSource                          = nullptr;

  UInt32              _transactionId                        = 0;
  void               *_balloonInflationSendBuffer           = nullptr;

  // Ballooned chunks sorted by base page frame number
  HyperVBalloonChunk **_chunks                              = nullptr;
  UInt32              _chunkCount                           = 0;
  UInt32              _chunkCapacity                        = 0;
  bool                _useLargeChunks                       = true;

  // Ballooned pages grabbed from the kernel free page queues
  HyperVBalloonPageIndex _grabbedPages;
  // No more pages are grabbed for the current inflation request after this uptime, 0 before its first grabbed page
  UInt64              _grabDeadline                         = 0;
  // 2 MB chunks are not tried before this uptime after one could not be allocated
  UInt64              _largeChunkRetryDeadline              = 0;

  void handlePacket(VMBusPacketHeader *pktHeader, UInt32 pktHeaderLength, UInt8 *pktData, UInt32 pktDataLength);
  void handleBalloonInflationRequest(HyperVDynamicMemoryMessageBalloonInflationRequest *request);
  void handleBalloonDeflationRequest(HyperVDynamicMemoryMessageBalloonDeflationRequest *request, UInt32 messageLength);
  void handleHotAddRequest(HyperVDynamicMemoryMessageHotAddRequest *request);
  void handleInformationMessage(HyperVDynamicMemoryMessageInformation *info);
  bool setupBalloon();
  bool doProtocolNegotitation(HyperVDynamicMemoryProtocolVersion version, bool isLastAttempt);
  IOReturn sendStatusReport(void*, void*, void*);
  bool inflateBalloon(UInt32 *pageCount);
  bool addGrabbedPage(HyperVDynamicMemoryMessage *response);
  bool shouldUseLargeChunks();
  HyperVBalloonChunk *allocateChunk(UInt32 pageCount);
  void freeChunk(HyperVBalloonChunk *chunk);
  bool findChunk(UInt64 pageFrameNumber, UInt32 *index);
  bool insertChunk(HyperVBalloonChunk *chunk);
  void removeChunk(UInt32 index);
  static UInt64 getPhysicalMemorySizeInPages();
  static UInt64 getBalloonFloorPageCount();
  bool getPagesStatus(UInt64 *committedPages, UInt64 *usingPages);

public:
  virtual bool start(IOService* provider) APPLE_KEXT_OVERRIDE;
  virtual void stop(IOService* provider) APPLE_KEXT_OVERRIDE;
};
#endif /* HyperVBalloon_hpp */
