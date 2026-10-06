//
//  HyperVBalloonAccounting.hpp
//  Hyper-V balloon kernel memory accounting
//
//  Copyright © 2026 xdqi. All rights reserved.
//

#ifndef HyperVBalloonAccounting_hpp
#define HyperVBalloonAccounting_hpp

#include <kern/locks.h>
#include <Headers/kern_patcher.hpp>

#include "HyperV.hpp"

//
// Hides ballooned pages from the kernel memory statistics, like Linux removes them from MemTotal.
// Ballooned pages stay wired by their memory descriptors, only the kernel counters are adjusted,
// the same way the kernel itself does not count wired compressor pool pages as wired.
//
class HyperVBalloonAccounting {
  HVDeclareLogFunctionsNonIOKit("ball", "HyperVBalloonAccounting");

private:
  //
  // Global instance.
  //
  static HyperVBalloonAccounting *_instance;

  //
  // Kernel globals, resolved through Lilu.
  //
  lck_mtx_t     *_vmPageQueueLock       = nullptr;
  unsigned int  *_vmPageWireCount       = nullptr;
  UInt64        *_maxMem                = nullptr;
  UInt64        *_maxMemActual          = nullptr;
  UInt64        *_machineInfoMaxMem     = nullptr;
  UInt64        *_vmGlobalUserWireLimit = nullptr;

  bool          _isEnabled              = false;

  //
  // Hidden memory, only changed with the page queue lock held.
  // The user wire limit may be lowered below the hidden memory size, so it is tracked separately.
  //
  UInt32        _hiddenPageCount        = 0;
  UInt64        _hiddenWireLimitBytes   = 0;

  //
  // Initialization function.
  //
  void init();
  void onLiluPatcherLoad(KernelPatcher &patcher);

  bool hide(UInt32 pageCount);
  void unhide(UInt32 pageCount);

public:
  //
  // Instance creator, must only be called on Lilu plugin start.
  //
  static HyperVBalloonAccounting *getInstance() {
    if (_instance == nullptr) {
      _instance = new HyperVBalloonAccounting;
      if (_instance != nullptr) {
        _instance->init();
      }
    }

    return _instance;
  }

  //
  // Hides wired pages from the kernel memory statistics, returns false if they are still counted.
  //
  static bool hidePages(UInt32 pageCount) {
    return _instance != nullptr && _instance->hide(pageCount);
  }

  //
  // Counts previously hidden pages again, must be called before the pages are unwired.
  //
  static void unhidePages(UInt32 pageCount) {
    if (_instance != nullptr) {
      _instance->unhide(pageCount);
    }
  }

  static UInt32 getHiddenPageCount() {
    return _instance != nullptr ? _instance->_hiddenPageCount : 0;
  }
};

#endif
