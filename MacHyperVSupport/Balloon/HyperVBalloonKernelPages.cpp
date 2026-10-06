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
// Keeps the compiler from moving the function pointer stores after the flag that publishes them.
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
  if (_getPhysPage == nullptr || _setOffset == nullptr || _getOffset == nullptr) {
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

HyperVKernelPageRef HyperVBalloonKernelPages::grab() {
  for (UInt32 attempt = 0; attempt < kHyperVBalloonGrabAttemptCount; ++attempt) {
    // Threads that are not VM privileged get no page below the free page reserve
    HyperVKernelPageRef page = _grab(0);
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
  return _getPhysPage(page);
}

vm_object_offset_t HyperVBalloonKernelPages::getOffset(HyperVKernelPageRef page) {
  return _getOffset(page);
}

void HyperVBalloonKernelPages::setOffset(HyperVKernelPageRef page, vm_object_offset_t offset) {
  _setOffset(page, offset);
}
