//
//  HyperVBalloonKernelPages.cpp
//  Hyper-V balloon kernel page allocation
//
//  Copyright © 2026 xdqi. All rights reserved.
//

#include "HyperVBalloonKernelPages.hpp"

#include <IOKit/IOLib.h>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>

HyperVBalloonKernelPages *HyperVBalloonKernelPages::_instance;

//
// Times a page is grabbed before giving up, and how long to wait in between for the pageout daemon to free pages.
//
#define kHyperVBalloonGrabAttemptCount        50
#define kHyperVBalloonGrabRetryIntervalMS     10

//
// struct vm_page of 10.6 (xnu-1456.1.26 to xnu-1504.15.3 osfmk/vm/vm_page.h), whose kernels before 10.6.8
// have no page accessors. Checked against the 10.6.0 kernel: vm_page_zero_fill() reads phys_page,
// vm_page_lookup() compares offset and pmap_startup() steps through vm_pages by the structure size.
//
#if defined(__i386__)
#define kHyperVSnowLeopardPageSize            0x2C
#define kHyperVSnowLeopardPageOffsetOffset    0x18
#define kHyperVSnowLeopardPagePhysPageOffset  0x24
#else
#define kHyperVSnowLeopardPageSize            0x48
#define kHyperVSnowLeopardPageOffsetOffset    0x30
#define kHyperVSnowLeopardPagePhysPageOffset  0x3C
#endif

//
// Number of pages spread over the 10.6 page array that are checked against the layout,
// and the least number of pages the page array must have for the check to mean anything.
//
#define kHyperVSnowLeopardPageCheckCount      65536
#define kHyperVSnowLeopardPageMinCount        4096

static inline UInt8 *getSnowLeopardPageField(HyperVKernelPageRef page, size_t offset) {
  return reinterpret_cast<UInt8 *>(page) + offset;
}

//
// Keeps the compiler from moving the stores of the kernel functions and the page layout after the flag that publishes them.
//
static inline void HyperVCompilerBarrier() {
  __asm__ volatile ("" ::: "memory");
}

void HyperVBalloonKernelPages::init() {
  HVCheckDebugArgs();

  //
  // The page functions are only known to have these signatures since 10.6.
  //
  if (getKernelVersion() < KernelVersion::SnowLeopard) {
    HVDBGLOG("Ballooned pages are not grabbed on this kernel");
    return;
  }
  if (checkKernelArgument("-hvballnograb")) {
    HVSYSLOG("Ballooned pages are not grabbed due to boot arg");
    return;
  }

  lilu.onPatcherLoadForce([](void *user, KernelPatcher &patcher) {
    static_cast<HyperVBalloonKernelPages *>(user)->onLiluPatcherLoad(patcher);
  }, this);
}

void HyperVBalloonKernelPages::onLiluPatcherLoad(KernelPatcher &patcher) {
  //
  // vm_page_grab_options() exists since 10.12, vm_page_grab() is inline since macOS 15.
  //
  _grab = reinterpret_cast<GrabFunction>(solveSymbol(patcher, "_vm_page_grab_options"));
  if (_grab == nullptr) {
    _grab = reinterpret_cast<GrabFunction>(solveSymbol(patcher, "_vm_page_grab"));
  }
  _release = reinterpret_cast<ReleaseFunction>(solveSymbol(patcher, "_vm_page_release"));
  if (_grab == nullptr || _release == nullptr) {
    HVSYSLOG("Failed to resolve kernel page functions, ballooned pages are not grabbed");
    return;
  }

  //
  // Page accessors exist since 10.6.8, the page structure differs between kernels.
  //
  _getPhysPage = reinterpret_cast<GetPhysPageFunction>(solveSymbol(patcher, "_vm_page_get_phys_page"));
  _setOffset   = reinterpret_cast<SetOffsetFunction>(solveSymbol(patcher, "_vm_page_set_offset"));
  _getOffset   = reinterpret_cast<GetOffsetFunction>(solveSymbol(patcher, "_vm_page_get_offset"));
  if (_getPhysPage == nullptr && _setOffset == nullptr && _getOffset == nullptr && getKernelVersion() == KernelVersion::SnowLeopard) {
    //
    // 10.6 kernels before 10.6.8 have none of them, but all 10.6 kernels share one page structure.
    //
    if (!useSnowLeopardLayout(patcher)) {
      return;
    }
  } else if (_getPhysPage == nullptr || _setOffset == nullptr || _getOffset == nullptr) {
    HVSYSLOG("Failed to resolve kernel page accessors, ballooned pages are not grabbed");
    return;
  }

  HyperVCompilerBarrier();
  _canGrab = true;
  HVDBGLOG("Ballooned pages are grabbed from the free page queues");
}

mach_vm_address_t HyperVBalloonKernelPages::solveSymbol(KernelPatcher &patcher, const char *symbol) {
  mach_vm_address_t address = patcher.solveSymbol(KernelPatcher::KernelID, symbol);
  patcher.clearError();
  return address;
}

bool HyperVBalloonKernelPages::useSnowLeopardLayout(KernelPatcher &patcher) {
  //
  // vm_pages holds the pages the kernel manages from startup on. pmap_startup() fills it in the order of
  // pmap_next_page() (xnu-1456.1.26 osfmk/i386/i386_vm_init.c:574), ascending as long as the firmware memory
  // map is sorted.
  //
  UInt8        **vmPages      = reinterpret_cast<UInt8 **>(solveSymbol(patcher, "_vm_pages"));
  unsigned int *vmPagesCount = reinterpret_cast<unsigned int *>(solveSymbol(patcher, "_vm_pages_count"));
  if (vmPages == nullptr || vmPagesCount == nullptr || *vmPages == nullptr || *vmPagesCount < kHyperVSnowLeopardPageMinCount) {
    HVSYSLOG("Kernel page array not found or too small, ballooned pages are not grabbed");
    return false;
  }
  _snowLeopardPages     = *vmPages;
  _snowLeopardPageCount = *vmPagesCount;

  //
  // A wrong layout reads pointers, flags or offsets instead, which are never in ascending order.
  // Check pages spread over the whole array, always including the last one.
  //
  UInt32  lastIndex        = _snowLeopardPageCount - 1;
  UInt32  step             = _snowLeopardPageCount / kHyperVSnowLeopardPageCheckCount + 1;
  UInt32  index            = 0;
  ppnum_t previousPhysPage = 0;
  while (true) {
    ppnum_t physPage = getSnowLeopardArrayPhysPage(index);
    if (index > 0 && physPage <= previousPhysPage) {
      HVSYSLOG("Kernel page %u at index %u follows page %u, ballooned pages are not grabbed", physPage, index, previousPhysPage);
      return false;
    }
    previousPhysPage = physPage;

    if (index == lastIndex) {
      break;
    }
    index = lastIndex - index > step ? index + step : lastIndex;
  }

  _usesSnowLeopardLayout = true;
  HVDBGLOG("Using the 10.6 page structure for %u kernel pages", _snowLeopardPageCount);
  return true;
}

ppnum_t HyperVBalloonKernelPages::getSnowLeopardArrayPhysPage(UInt32 index) {
  HyperVKernelPageRef page = reinterpret_cast<HyperVKernelPageRef>(_snowLeopardPages + (size_t) index * kHyperVSnowLeopardPageSize);
  return *reinterpret_cast<ppnum_t *>(getSnowLeopardPageField(page, kHyperVSnowLeopardPagePhysPageOffset));
}

bool HyperVBalloonKernelPages::findSnowLeopardArrayPhysPage(ppnum_t physPage) {
  UInt32 low  = 0;
  UInt32 high = _snowLeopardPageCount;
  while (low < high) {
    UInt32  middle         = low + (high - low) / 2;
    ppnum_t middlePhysPage = getSnowLeopardArrayPhysPage(middle);
    if (middlePhysPage == physPage) {
      return true;
    }
    if (middlePhysPage < physPage) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return false;
}

bool HyperVBalloonKernelPages::isSnowLeopardFreePage(HyperVKernelPageRef page) {
  //
  // Free pages belong to no VM object and have an offset of -1 (xnu-1456.1.26 osfmk/vm/vm_resident.c:491, :1308),
  // which also checks where the offset is.
  //
  vm_object_offset_t offset = *reinterpret_cast<vm_object_offset_t *>(getSnowLeopardPageField(page, kHyperVSnowLeopardPageOffsetOffset));
  if (offset != (vm_object_offset_t) -1) {
    return false;
  }

  //
  // Pages of the page array must start at a page structure. Pages the kernel frees from its own image and the
  // boot data after startup get a page structure of their own (vm_page_create(), xnu-1456.1.26
  // osfmk/vm/vm_resident.c:960), their physical pages are not in the page array.
  //
  uintptr_t address = reinterpret_cast<uintptr_t>(page);
  uintptr_t start   = reinterpret_cast<uintptr_t>(_snowLeopardPages);
  if (address >= start && (address - start) / kHyperVSnowLeopardPageSize < _snowLeopardPageCount) {
    return (address - start) % kHyperVSnowLeopardPageSize == 0;
  }

  ppnum_t physPage = getPhysPage(page);
  return physPage != 0 && physPage <= getSnowLeopardArrayPhysPage(_snowLeopardPageCount - 1)
    && !findSnowLeopardArrayPhysPage(physPage);
}

HyperVKernelPageRef HyperVBalloonKernelPages::grab() {
  for (UInt32 attempt = 0; attempt < kHyperVBalloonGrabAttemptCount; ++attempt) {
    // Threads that are not VM privileged get no page below the free page reserve
    HyperVKernelPageRef page = _grab(0);
    if (page != nullptr && _usesSnowLeopardLayout && !isSnowLeopardFreePage(page)) {
      //
      // The page structure is not the expected one, hand the page back without changing it.
      //
      _canGrab = false;
      _release(page, 0);
      HVSYSLOG("Grabbed page %p does not match the 10.6 page structure, no longer grabbing pages", page);
      return nullptr;
    }
    if (page != nullptr) {
      return page;
    }

    //
    // Grabbing below the free page minimum woke up the pageout daemon, which frees pages, mostly from the file cache.
    // Wait for it like kernel_memory_allocate() does, but only for a while, there may be nothing left to free and
    // deflation requests and status reports must not be stalled for long.
    //
    IOSleep(kHyperVBalloonGrabRetryIntervalMS);
  }

  return nullptr;
}

void HyperVBalloonKernelPages::releasePage(HyperVKernelPageRef page) {
  // Free pages belong to no VM object and have an offset of -1, which the kernel checks
  _instance->setOffset(page, (vm_object_offset_t) -1);
  _instance->_release(page, 0);
}

ppnum_t HyperVBalloonKernelPages::getPhysPage(HyperVKernelPageRef page) {
  if (_usesSnowLeopardLayout) {
    return *reinterpret_cast<ppnum_t *>(getSnowLeopardPageField(page, kHyperVSnowLeopardPagePhysPageOffset));
  }
  return _getPhysPage(page);
}

vm_object_offset_t HyperVBalloonKernelPages::getOffset(HyperVKernelPageRef page) {
  if (_usesSnowLeopardLayout) {
    return *reinterpret_cast<vm_object_offset_t *>(getSnowLeopardPageField(page, kHyperVSnowLeopardPageOffsetOffset));
  }
  return _getOffset(page);
}

void HyperVBalloonKernelPages::setOffset(HyperVKernelPageRef page, vm_object_offset_t offset) {
  if (_usesSnowLeopardLayout) {
    *reinterpret_cast<vm_object_offset_t *>(getSnowLeopardPageField(page, kHyperVSnowLeopardPageOffsetOffset)) = offset;
    return;
  }
  _setOffset(page, offset);
}
