//
//  HyperVBalloonPrivate.cpp
//  Hyper-V balloon driver internal implementation
//
//  Copyright © 2022-2023 xdqi. All rights reserved.
//

#include <string.h>
#include <mach/vm_statistics.h>
#include <sys/sysctl.h>

#include "HyperVBalloon.hpp"
#include "HyperVBalloonAccounting.hpp"
#include "HyperVBalloonRegs.hpp"

bool HyperVBalloon::setupBalloon() {
  // Protocol version negotiation
  if (!doProtocolNegotitation(kHyperVDynamicMemoryProtocolVersionWin10, false) &&
      !doProtocolNegotitation(kHyperVDynamicMemoryProtocolVersionWin8, false) &&
      !doProtocolNegotitation(kHyperVDynamicMemoryProtocolVersionWin7, true)) {
    return false;
  }
  
  // Report capabilities to hypervisor
  HyperVDynamicMemoryMessage message, protoResponse;
  memset(&message, 0, sizeof(HyperVDynamicMemoryMessageHeader) + sizeof(HyperVDynamicMemoryMessageCapabilitiesReport));
  message.header.type = kDynamicMemoryMessageTypeCapabilitiesReport;
  message.header.size = sizeof(HyperVDynamicMemoryMessageHeader) + sizeof(HyperVDynamicMemoryMessageCapabilitiesReport);
  OSIncrementAtomic(&_transactionId);
  message.header.transactionId = _transactionId;
  message.capabilitiesReport.supportBalloon    = 1;
  //
  // macOS doesn't support memory hot adding, but the host does not issue balloon requests
  // unless hot-add is advertised. Hot-add requests are refused in handleHotAddRequest().
  // Alignment is log2 of the hot-add chunk size in MB (128 MB, same as Linux without memory hotplug).
  //
  message.capabilitiesReport.supportHotAdd     = 1;
  message.capabilitiesReport.hotAddAlignment   = 7;
  message.capabilitiesReport.minimumPageCount  = 0;  // the same as
  message.capabilitiesReport.maximumPageNumber = -1; // both Windows and Linux drivers
    
  HVDBGLOG("Posting dynamic memory capatibilities report transactionID %u", _transactionId);
  if (_hvDevice->writeInbandPacketWithTransactionId(&message, message.header.size, _transactionId, true, &protoResponse, sizeof(protoResponse)) != kIOReturnSuccess) {
    return false;
  }

  HVDBGLOG("Got dynamic memory capatibilities response of %u", protoResponse.capabilitiesResponse.isAccepted);
  if (!protoResponse.capabilitiesResponse.isAccepted) {
    return false;
  }

  return true;
}

bool HyperVBalloon::doProtocolNegotitation(HyperVDynamicMemoryProtocolVersion version, bool isLastAttempt) {
  HyperVDynamicMemoryMessage message, protoResponse;
  memset(&message, 0, sizeof(HyperVDynamicMemoryMessageHeader) + sizeof(HyperVDynamicMemoryMessageProtocolRequest));
  message.header.type = kDynamicMemoryMessageTypeProtocolRequest;
  message.header.size = sizeof(HyperVDynamicMemoryMessageHeader) + sizeof(HyperVDynamicMemoryMessageProtocolRequest);
  OSIncrementAtomic(&_transactionId);
  message.header.transactionId = _transactionId;
  message.protocolRequest.version = version;
  message.protocolRequest.isLastAttempt = isLastAttempt;
    
  HVDBGLOG("Trying with dynamic memory protocol of %u.%u transactionID %u", version >> 16, version & 0xffff, _transactionId);
  if (_hvDevice->writeInbandPacketWithTransactionId(&message, message.header.size, _transactionId, true, &protoResponse, sizeof(protoResponse)) != kIOReturnSuccess) {
    return false;
  }

  HVDBGLOG("Got dynamic memory protocol response of %u", protoResponse.protocolResponse.isAccepted);
  if (!protoResponse.protocolResponse.isAccepted) {
    return false;
  }
  
  HVDBGLOG("Using dynamic memory protocol version of %u.%u", version >> 16, version & 0xffff);
  return true;
}

void HyperVBalloon::handlePacket(VMBusPacketHeader *pktHeader, UInt32 pktHeaderLength, UInt8 *pktData, UInt32 pktDataLength) {
  HyperVDynamicMemoryMessage *balloonMessage = (HyperVDynamicMemoryMessage*) pktData;
  
  switch (balloonMessage->header.type) {
    case kDynamicMemoryMessageTypeProtocolResponse:
    case kDynamicMemoryMessageTypeCapabilitiesResponse:
      // Pass them to HyperVBalloon::start()
      void   *responseBuffer;
      UInt32 responseLength;

      //
      // The protocol response packet always has a transaction ID of 0, wake using fixed transaction ID.
      //
      if (_hvDevice->getPendingTransaction(_transactionId, &responseBuffer, &responseLength)) {
        if (pktDataLength < responseLength) {
          responseLength = pktDataLength;
        }
        memcpy(responseBuffer, pktData, responseLength);
        _hvDevice->wakeTransaction(_transactionId);
      }
      break;

    case kDynamicMemoryMessageTypeBalloonInflationRequest:
      handleBalloonInflationRequest(&balloonMessage->inflationRequest);
      break;

    case kDynamicMemoryMessageTypeBalloonDeflationRequest:
      handleBalloonDeflationRequest(&balloonMessage->deflationRequest);
      break;

    case kDynamicMemoryMessageTypeHotAddRequest:
      handleHotAddRequest(&balloonMessage->hotAddRequest);
      break;

    case kDynamicMemoryMessageTypeInfoMessage:
      handleInformationMessage(&balloonMessage->information);
      break;
      
    default:
      HVDBGLOG("Unknown dynamic memory message type %u, size %u", balloonMessage->header.type, balloonMessage->header.size);
      break;
  }
}

UInt64 HyperVBalloon::getPhysicalMemorySizeInPages() {
  UInt64 totalMemorySize = 0;
  size_t totalMemorySizeSize = sizeof(totalMemorySize);
  sysctlbyname("hw.memsize", &totalMemorySize, &totalMemorySizeSize, nullptr, 0);
  // Hidden ballooned pages are not part of hw.memsize
  return totalMemorySize / PAGE_SIZE + HyperVBalloonAccounting::getHiddenPageCount();
}

//
// Memory that is never ballooned out, a continuous piecewise linear function of total memory (same as Linux):
//
//   total MB -> floor MB    gradient
//         0          0
//        16         16
//        32         24
//       128         72        (1/2)
//       512        168        (1/4)
//      2048        360        (1/8)
//      8192        744       (1/16)
//     32768       1512       (1/32)
//
UInt64 HyperVBalloon::getBalloonFloorPageCount() {
  UInt64 totalPages = getPhysicalMemorySizeInPages();

#define MB2PAGES(mb) (((UInt64) (mb)) << (20 - PAGE_SHIFT))
  UInt64 floorPages;
  if (totalPages < MB2PAGES(128)) {
    floorPages = MB2PAGES(8) + (totalPages >> 1);
  } else if (totalPages < MB2PAGES(512)) {
    floorPages = MB2PAGES(40) + (totalPages >> 2);
  } else if (totalPages < MB2PAGES(2048)) {
    floorPages = MB2PAGES(104) + (totalPages >> 3);
  } else if (totalPages < MB2PAGES(8192)) {
    floorPages = MB2PAGES(232) + (totalPages >> 4);
  } else {
    floorPages = MB2PAGES(488) + (totalPages >> 5);
  }
#undef MB2PAGES

  return floorPages;
}

IOReturn HyperVBalloon::sendStatusReport(void*, void*, void*) {
  HyperVDynamicMemoryMessage message;
  memset(&message, 0, sizeof(HyperVDynamicMemoryMessageHeader) + sizeof(HyperVDynamicMemoryMessageStatusReport));
  message.header.type = kDynamicMemoryMessageTypeStatusReport;
  message.header.size = sizeof(HyperVDynamicMemoryMessageHeader) + sizeof(HyperVDynamicMemoryMessageStatusReport);
  OSIncrementAtomic(&_transactionId);
  message.header.transactionId = _transactionId;

  UInt64 totalPages = getPhysicalMemorySizeInPages();
  UInt64 committedPages, usingPages;
  getPagesStatus(&committedPages, &usingPages);
  // We just post two fields below, other fields are not needed by hypervisor
  // Clamp to zero, a wrapped value would make the host balloon out even more memory
  message.statusReport.availablePages = totalPages > usingPages ? totalPages - usingPages : 0;
  //
  // The host computes memory pressure from committed memory only. Ballooned pages are wired and thus
  // already committed, like Linux also report the floor as committed so the host leaves it to the guest.
  //
  message.statusReport.committedPages = committedPages + getBalloonFloorPageCount();

  HVDATADBGLOG("Posting memory status report: available %llu, committed %llu", message.statusReport.availablePages, message.statusReport.committedPages);

  // schedule next run
  _timerSource->setTimeoutMS(kHyperVDynamicMemoryStatusReportIntervalMilliseconds);
  
  return _hvDevice->writeInbandPacket(&message, sizeof(HyperVDynamicMemoryMessageHeader) + sizeof(HyperVDynamicMemoryMessageStatusReport), false);
}

void HyperVBalloon::getPagesStatus(UInt64 *committedPages, UInt64 *usingPages) {
  static mach_msg_type_name_t vmStatSize;
#if defined(__i386__)
  static vm_statistics_data_t vmStat;
  vmStatSize = HOST_VM_INFO_COUNT;
  HyperVBalloonAccounting::getVMStatistics(&vmStat, &vmStatSize);
#elif defined(__x86_64__)
  static vm_statistics64_data_t vmStat;
  vmStatSize = HOST_VM_INFO64_COUNT;
  HyperVBalloonAccounting::getVMStatistics(&vmStat, &vmStatSize);
#else
#error Unsupported arch
#endif

  // wire_count:            "Wired Memory", pages that cannot be swapped out, e.g. kernel memory
  // internal_page_count:   "App Memory"
  // compressor_page_count: "Compressed Memory"
  // active_page_count:     pages that is actively used
  // Hidden ballooned pages are still wired, but not part of wire_count
  UInt64 wiredPages = (UInt64) vmStat.wire_count + HyperVBalloonAccounting::getHiddenPageCount();
  if (committedPages) {
#if defined(__i386__)
    *committedPages = wiredPages + vmStat.active_count;
#elif defined(__x86_64__)
    *committedPages = wiredPages + vmStat.internal_page_count + vmStat.compressor_page_count;
#endif
  }
  if (usingPages) {
    *usingPages = wiredPages + vmStat.active_count
#if defined(__x86_64__)
    + vmStat.compressor_page_count
#endif
    ;
  }
}

void HyperVBalloon::handleHotAddRequest(HyperVDynamicMemoryMessageHotAddRequest *request) {
  HyperVDynamicMemoryMessage message;
  memset(&message, 0, sizeof(HyperVDynamicMemoryMessageHeader) + sizeof(HyperVDynamicMemoryMessageHotAddResponse));
  message.header.type = kDynamicMemoryMessageTypeHotAddResponse;
  message.header.size = sizeof(HyperVDynamicMemoryMessageHeader) + sizeof(HyperVDynamicMemoryMessageHotAddResponse);
  OSIncrementAtomic(&_transactionId);
  message.header.transactionId = _transactionId;
  
  HVDBGLOG("Receiving hot add request");
  
  // Success with no pages added tells the host that hot-add failed permanently, so it will not retry.
  message.hotAddResponse.result = 1;
  message.hotAddResponse.pageCount = 0;
  
  HVDBGLOG("Sending hot add response");
  if (_hvDevice->writeInbandPacket(&message, sizeof(HyperVDynamicMemoryMessageHeader) + sizeof(HyperVDynamicMemoryMessageHotAddResponse), false) != kIOReturnSuccess) {
    HVSYSLOG("Hot add response send failed");
  }
}

void HyperVBalloon::handleInformationMessage(HyperVDynamicMemoryMessageInformation *info) {
  switch (info->type) {
    case kHyperVDynamicMemoryInformationTypeMaximumPageCount:
      if (info->dataSize == sizeof(UInt64)) {
        HVDBGLOG("Received information: maximum dynamic memory size is %lluMB", info->number >> (20 - PAGE_SHIFT));
      }
      break;
      
    default:
      HVDBGLOG("Received unknown information type %u", info->type);
      break;
  }
}

HyperVBalloonChunk *HyperVBalloon::allocateChunk(UInt32 pageCount) {
  //
  // Multi-page chunks are physically contiguous and aligned to their size,
  // so each chunk can be reported to the host as a single page range.
  //
  IOByteCount  chunkSize = (IOByteCount) pageCount * PAGE_SIZE;
  IOOptionBits options   = kIOMemoryMapperNone | (pageCount > 1 ? kIOMemoryPhysicallyContiguous : 0);
  IOBufferMemoryDescriptor *memoryDescriptor = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, options, chunkSize, chunkSize);
  if (memoryDescriptor == nullptr) {
    return nullptr;
  }

  IOByteCount physicalSegmentSize;
  addr64_t physicalSegmentStart = memoryDescriptor->getPhysicalSegment(0, &physicalSegmentSize, 0);
  HVDATADBGLOG("Allocated %u pages: desc %p, raw 0x%llX, size %llu", pageCount, memoryDescriptor, (UInt64) physicalSegmentStart, (UInt64) physicalSegmentSize);
  if (physicalSegmentSize < chunkSize) {
    HVSYSLOG("Balloon chunk at 0x%llX is not physically contiguous", (UInt64) physicalSegmentStart);
    memoryDescriptor->release();
    return nullptr;
  }

  HyperVBalloonChunk *chunk = static_cast<HyperVBalloonChunk *>(IOMalloc(sizeof(*chunk)));
  if (chunk == nullptr) {
    memoryDescriptor->release();
    return nullptr;
  }

  memset(chunk, 0, sizeof(*chunk));
  chunk->basePageFrameNumber = physicalSegmentStart >> PAGE_SHIFT;
  chunk->pageCount           = pageCount;
  chunk->memoryDescriptor    = memoryDescriptor;
  return chunk;
}

void HyperVBalloon::freeChunk(HyperVBalloonChunk *chunk) {
  chunk->memoryDescriptor->release();
  IOFree(chunk, sizeof(*chunk));
}

bool HyperVBalloon::findChunk(UInt64 pageFrameNumber, UInt32 *index) {
  //
  // Find the first chunk starting after the page frame number,
  // only the chunk before it can contain the page frame number.
  //
  UInt32 low  = 0;
  UInt32 high = _chunkCount;
  while (low < high) {
    UInt32 middle = low + (high - low) / 2;
    if (_chunks[middle]->basePageFrameNumber <= pageFrameNumber) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }

  *index = low;
  if (low > 0 && pageFrameNumber < _chunks[low - 1]->basePageFrameNumber + _chunks[low - 1]->pageCount) {
    *index = low - 1;
    return true;
  }
  return false;
}

bool HyperVBalloon::insertChunk(HyperVBalloonChunk *chunk) {
  UInt32 index;
  if (findChunk(chunk->basePageFrameNumber, &index)) {
    HVSYSLOG("Memory block %llu is already ballooned", chunk->basePageFrameNumber);
    return false;
  }

  if (_chunkCount == _chunkCapacity) {
    UInt32 newCapacity = _chunkCapacity * 2;
    HyperVBalloonChunk **newChunks = static_cast<HyperVBalloonChunk **>(IOMalloc(sizeof(*newChunks) * newCapacity));
    if (newChunks == nullptr) {
      return false;
    }
    memcpy(newChunks, _chunks, sizeof(*_chunks) * _chunkCount);
    IOFree(_chunks, sizeof(*_chunks) * _chunkCapacity);
    _chunks        = newChunks;
    _chunkCapacity = newCapacity;
  }

  memmove(&_chunks[index + 1], &_chunks[index], sizeof(*_chunks) * (_chunkCount - index));
  _chunks[index] = chunk;
  ++_chunkCount;
  return true;
}

void HyperVBalloon::removeChunk(UInt32 index) {
  memmove(&_chunks[index], &_chunks[index + 1], sizeof(*_chunks) * (_chunkCount - index - 1));
  --_chunkCount;
}

bool HyperVBalloon::inflateBalloon(UInt32 *pageCount) {
  HVDBGLOG("This time inflate up to %u pages", *pageCount);

  HyperVDynamicMemoryMessage *response = reinterpret_cast<HyperVDynamicMemoryMessage *>(_balloonInflationSendBuffer);
  memset(response, 0, kHyperVDynamicMemoryResponsePacketSize);

  // we fill size and transactionId just before sending
  response->header.type = kDynamicMemoryMessageTypeBalloonInflationResponse;

  // reset to 0 per every request
  response->inflationResponse.rangeCount = 0;

  //
  // Each ballooned chunk is reported as its own page range, 2 MB chunks are used when possible.
  // The host may return arbitrary parts of a chunk, it is freed once all of its pages are returned.
  //
  UInt32 balloonedPageCount = 0;
  bool   allocationFailed   = false;
  while (balloonedPageCount < *pageCount && response->inflationResponse.rangeCount < kHyperVDynamicMemoryInflationMaxRangeCount) {
    UInt32 remainingPageCount = *pageCount - balloonedPageCount;
    UInt32 chunkPageCount     = (_useLargeChunks && remainingPageCount >= kHyperVDynamicMemoryChunkPageCount) ? kHyperVDynamicMemoryChunkPageCount : 1;

    HyperVBalloonChunk *chunk = allocateChunk(chunkPageCount);
    if (chunk == nullptr && chunkPageCount > 1) {
      // Fall back to single pages for the rest of the request, like Linux
      HVDBGLOG("Failed to allocate a 2 MB balloon chunk, falling back to single pages");
      _useLargeChunks = false;
      continue;
    }
    if (chunk == nullptr) {
      HVSYSLOG("Failed to allocate balloon page");
      allocationFailed = true;
      break;
    }
    if (!insertChunk(chunk)) {
      // Not reported to the host yet, so it can be handed back to the kernel
      HVSYSLOG("Failed to track balloon page");
      freeChunk(chunk);
      allocationFailed = true;
      break;
    }
    chunk->isHidden = HyperVBalloonAccounting::hidePages(chunk->pageCount);

    UInt32 rangeIndex = response->inflationResponse.rangeCount;
    response->inflationResponse.ranges[rangeIndex].startPageFrameNumber = chunk->basePageFrameNumber;
    response->inflationResponse.ranges[rangeIndex].pageCount            = chunk->pageCount;
    ++response->inflationResponse.rangeCount;
    balloonedPageCount += chunk->pageCount;
  }

  // Stop inflating if the request could not be fully satisfied
  *pageCount -= balloonedPageCount;
  bool morePages = *pageCount > 0 && !allocationFailed;
  response->inflationResponse.morePages = morePages;

  UInt32 responseLength = sizeof(HyperVDynamicMemoryMessageHeader) + sizeof(HyperVDynamicMemoryMessageBalloonInflationResponse) + sizeof(HyperVDynamicMemoryPageRange) * response->inflationResponse.rangeCount;
  response->header.size = responseLength;
  
  // size = protocol header + inflation response header + memory ranges
  IOReturn status;
  do {
    OSIncrementAtomic(&_transactionId);
    response->header.transactionId = _transactionId;
    HVDBGLOG("Send inflation request with %u pages in %u ranges, length %u, transactionId %u", balloonedPageCount, (UInt32) response->inflationResponse.rangeCount, responseLength, _transactionId);

    status = _hvDevice->writeInbandPacket(response, responseLength, false);
    if (status != kIOReturnSuccess) {
      HVDBGLOG("Send inflation request failed, code %d", status);
      IOSleep(20);
    }
  } while (status != kIOReturnSuccess);

  return morePages;
}

void HyperVBalloon::handleBalloonInflationRequest(HyperVDynamicMemoryMessageBalloonInflationRequest *request) {
  UInt32 pageCount = request->pageCount;
  UInt64 totalPages = getPhysicalMemorySizeInPages();
  UInt64 usedPages;
  getPagesStatus(nullptr, &usedPages);
  UInt64 availablePages = totalPages > usedPages ? totalPages - usedPages : 0;

  UInt64 floorPages     = getBalloonFloorPageCount();

  HVDBGLOG("Received request to inflate %u pages", pageCount);

  // Never balloon below the floor
  if (availablePages < (UInt64) pageCount + floorPages) {
    HVSYSLOG("Balloon floor reached, filling a part of requested");
    pageCount = availablePages > floorPages ? (UInt32) (availablePages - floorPages) : 0;
  }

  // Try 2 MB chunks again for every request
  _useLargeChunks = true;

  // The host waits for a final response even if nothing could be ballooned
  bool morePages;
  do {
    morePages = inflateBalloon(&pageCount);
  } while (morePages);
}

void HyperVBalloon::handleBalloonDeflationRequest(HyperVDynamicMemoryMessageBalloonDeflationRequest *request) {
  for (UInt32 i = 0; i < request->rangeCount; ++i) {
    UInt64 pageFrameNumber    = request->ranges[i].startPageFrameNumber;
    UInt64 endPageFrameNumber = pageFrameNumber + request->ranges[i].pageCount;

    while (pageFrameNumber < endPageFrameNumber) {
      UInt32 index;
      if (!findChunk(pageFrameNumber, &index)) {
        HVSYSLOG("Memory block %llu not found", pageFrameNumber);
        ++pageFrameNumber;
        continue;
      }

      // Pages of a chunk can only go back to the kernel together, free it once the host returned all of them
      HyperVBalloonChunk *chunk = _chunks[index];
      UInt64 chunkEndPageFrameNumber    = chunk->basePageFrameNumber + chunk->pageCount;
      UInt64 returnedEndPageFrameNumber = endPageFrameNumber < chunkEndPageFrameNumber ? endPageFrameNumber : chunkEndPageFrameNumber;
      // Count every page once, even if the host returns it more than once
      UInt32 newlyReturnedPageCount = 0;
      for (UInt64 returnedPageFrameNumber = pageFrameNumber; returnedPageFrameNumber < returnedEndPageFrameNumber; ++returnedPageFrameNumber) {
        UInt32 pageIndex = (UInt32) (returnedPageFrameNumber - chunk->basePageFrameNumber);
        UInt64 pageBit   = 1ULL << (pageIndex % 64);
        if ((chunk->returnedPages[pageIndex / 64] & pageBit) == 0) {
          chunk->returnedPages[pageIndex / 64] |= pageBit;
          ++newlyReturnedPageCount;
        }
      }
      chunk->returnedPageCount += newlyReturnedPageCount;
      // Returned pages are backed again and must be counted before the chunk is freed
      if (chunk->isHidden && newlyReturnedPageCount > 0) {
        HyperVBalloonAccounting::unhidePages(newlyReturnedPageCount);
      }
      HVDATADBGLOG("Memory blocks %llu-%llu released", pageFrameNumber, returnedEndPageFrameNumber - 1);

      if (chunk->returnedPageCount >= chunk->pageCount) {
        removeChunk(index);
        freeChunk(chunk);
      }
      pageFrameNumber = returnedEndPageFrameNumber;
    }
  }

  // we should send response until the last deflation message received
  if (request->morePages) return;

  HyperVDynamicMemoryMessage message;
  // deflation response is empty so we don't add sizeof (which in fact is 1) to message size
  memset(&message, 0, sizeof(HyperVDynamicMemoryMessageHeader));
  message.header.type = kDynamicMemoryMessageTypeBalloonDeflationResponse;
  message.header.size = sizeof(HyperVDynamicMemoryMessageHeader);
  OSIncrementAtomic(&_transactionId);
  message.header.transactionId = _transactionId;

  HVDBGLOG("Sending deflation response");
  if (_hvDevice->writeInbandPacket(&message, sizeof(HyperVDynamicMemoryMessageHeader), false) != kIOReturnSuccess) {
    HVSYSLOG("Deflation response send failed");
  }
}
