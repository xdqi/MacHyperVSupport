//
//  HyperVBalloonAccounting.hpp
//  Hyper-V balloon kernel memory accounting
//
//  Copyright © 2026 xdqi. All rights reserved.
//

#ifndef HyperVBalloonAccounting_hpp
#define HyperVBalloonAccounting_hpp

#include <kern/host.h>
#include <kern/locks.h>
#include <mach/vm_statistics.h>
#include <Headers/kern_patcher.hpp>

#include "HyperV.hpp"

//
// Provides the kernel memory statistics, which are not exported to kexts.
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
  // Kernel functions and globals, resolved through Lilu.
  //
#if defined(__i386__)
  typedef kern_return_t (*HostStatisticsFunction)(host_t host, host_flavor_t flavor, host_info_t info, mach_msg_type_number_t *count);
#else
  typedef kern_return_t (*HostStatisticsFunction)(host_t host, host_flavor_t flavor, host_info64_t info, mach_msg_type_number_t *count);
#endif
  HostStatisticsFunction _hostStatistics = nullptr;

  lck_mtx_t     *_vmPageQueueLock       = nullptr;
  unsigned int  *_vmPageWireCount       = nullptr;
  UInt64        *_maxMem                = nullptr;
  UInt64        *_maxMemActual          = nullptr;
  UInt64        *_machineInfoMaxMem     = nullptr;
  UInt64        *_vmGlobalUserWireLimit = nullptr;

  bool          _canHide                = false;
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
  static mach_vm_address_t solveSymbol(KernelPatcher &patcher, const char *symbol);
  void enableHiding(KernelPatcher &patcher);

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
  // Gets the kernel VM statistics, HOST_VM_INFO64 on 64-bit kernels and HOST_VM_INFO on 32-bit kernels.
  //
#if defined(__i386__)
  static kern_return_t getVMStatistics(vm_statistics_t statistics, mach_msg_type_number_t *count);
#else
  static kern_return_t getVMStatistics(vm_statistics64_t statistics, mach_msg_type_number_t *count);
#endif

  static bool canGetVMStatistics() {
    return _instance != nullptr && _instance->_hostStatistics != nullptr;
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
