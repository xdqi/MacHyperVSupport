//
//  HyperVBalloonKernelPages.hpp
//  Hyper-V balloon kernel page allocation
//
//  Copyright © 2026 xdqi. All rights reserved.
//

#ifndef HyperVBalloonKernelPages_hpp
#define HyperVBalloonKernelPages_hpp

#include <mach/vm_types.h>
#include <Headers/kern_patcher.hpp>

#include "HyperV.hpp"

//
// Kernel page, struct vm_page (osfmk/vm/vm_page.h), which is not part of the SDK.
//
typedef struct HyperVKernelPage *HyperVKernelPageRef;

//
// Takes pages straight from the kernel free page queues, the kernel functions for that are not exported to kexts.
//
// Grabbed pages are not mapped, not wired and on no page queue, like pages the kernel has just taken
// for a VM object. Unlike memory descriptors, they need no kernel address space and no allocations
// of their own. Their offset field is unused until they are put into a VM object, so it links them.
//
class HyperVBalloonKernelPages {
  HVDeclareLogFunctionsNonIOKit("ball", "HyperVBalloonKernelPages");

private:
  //
  // Global instance.
  //
  static HyperVBalloonKernelPages *_instance;

  //
  // Kernel functions, resolved through Lilu. They are never cleared once resolved, so grabbed pages can always
  // be released. vm_page_grab_options() takes options since 10.12. vm_page_release() takes whether the page
  // queue lock is held on 10.12 to 14 and options since macOS 15. Zero is right for all of them, older kernels
  // ignore the extra argument.
  //
  typedef HyperVKernelPageRef (*GrabFunction)(int options);
  typedef void (*ReleaseFunction)(HyperVKernelPageRef page, int options);
  typedef ppnum_t (*GetPhysPageFunction)(HyperVKernelPageRef page);
  typedef void (*SetOffsetFunction)(HyperVKernelPageRef page, vm_object_offset_t offset);
  typedef vm_object_offset_t (*GetOffsetFunction)(HyperVKernelPageRef page);

  GrabFunction          _grab        = nullptr;
  ReleaseFunction       _release     = nullptr;
  GetPhysPageFunction   _getPhysPage = nullptr;
  SetOffsetFunction     _setOffset   = nullptr;
  GetOffsetFunction     _getOffset   = nullptr;

  bool                  _canGrab     = false;

  //
  // Initialization function.
  //
  void init();
  void onLiluPatcherLoad(KernelPatcher &patcher);
  static mach_vm_address_t solveSymbol(KernelPatcher &patcher, const char *symbol);

  HyperVKernelPageRef grab();
  ppnum_t getPhysPage(HyperVKernelPageRef page);
  vm_object_offset_t getOffset(HyperVKernelPageRef page);
  void setOffset(HyperVKernelPageRef page, vm_object_offset_t offset);

public:
  //
  // Instance creator, must only be called on Lilu plugin start.
  //
  static HyperVBalloonKernelPages *getInstance() {
    if (_instance == nullptr) {
      _instance = new HyperVBalloonKernelPages;
      if (_instance != nullptr) {
        _instance->init();
      }
    }

    return _instance;
  }

  //
  // Returns true if pages can be grabbed. Grabbed pages can always be released.
  //
  static bool canGrabPages() {
    return _instance != nullptr && _instance->_canGrab;
  }

  //
  // Takes a page from the free page queues. Below the free page reserve, waits a while for the pageout daemon
  // to free pages. Returns nullptr if no page could be taken.
  //
  static HyperVKernelPageRef grabPage() {
    return canGrabPages() ? _instance->grab() : nullptr;
  }

  //
  // Hands a grabbed page back to the free page queues, must not be called with the page queue lock held.
  //
  static void releasePage(HyperVKernelPageRef page);

  static ppnum_t getPageFrameNumber(HyperVKernelPageRef page) {
    return _instance->getPhysPage(page);
  }

  //
  // Links grabbed pages through their offset field. A page fresh off the free page queues has no link yet.
  //
  static HyperVKernelPageRef getLink(HyperVKernelPageRef page) {
    return reinterpret_cast<HyperVKernelPageRef>(static_cast<uintptr_t>(_instance->getOffset(page)));
  }

  static void setLink(HyperVKernelPageRef page, HyperVKernelPageRef link) {
    _instance->setOffset(page, reinterpret_cast<uintptr_t>(link));
  }
};

#endif
