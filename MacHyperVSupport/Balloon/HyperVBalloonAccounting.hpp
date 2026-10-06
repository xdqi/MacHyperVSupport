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
// Only the kernel counters are adjusted. Pages ballooned through memory descriptors stay wired and are
// removed from the wired page count, the same way the kernel itself does not count wired compressor pool
// pages as wired. Pages grabbed from the free page queues are neither wired nor on any page queue, so they are in no page count.
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
  // Hidden wired pages are the part of the hidden pages that was taken from the wired page count.
  // The user wire limit may be lowered below the hidden memory size, so it is tracked separately.
  //
  UInt32        _hiddenPageCount        = 0;
  UInt32        _hiddenWiredPageCount   = 0;
  UInt64        _hiddenWireLimitBytes   = 0;

  //
  // Initialization function.
  //
  void init();
  void onLiluPatcherLoad(KernelPatcher &patcher);
  static mach_vm_address_t solveSymbol(KernelPatcher &patcher, const char *symbol);
  void enableHiding(KernelPatcher &patcher);

  bool hide(UInt32 pageCount, bool isWired);
  void unhide(UInt32 pageCount, bool isWired);

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
  // Hides ballooned pages from the kernel memory statistics, returns false if they are still counted.
  // Wired pages are also taken from the wired page count.
  //
  static bool hidePages(UInt32 pageCount, bool isWired) {
    return _instance != nullptr && _instance->hide(pageCount, isWired);
  }

  //
  // Counts previously hidden pages again, must be called before the pages are unwired or released.
  //
  static void unhidePages(UInt32 pageCount, bool isWired) {
    if (_instance != nullptr) {
      _instance->unhide(pageCount, isWired);
    }
  }

  static UInt32 getHiddenPageCount() {
    return _instance != nullptr ? _instance->_hiddenPageCount : 0;
  }

  static UInt32 getHiddenWiredPageCount() {
    return _instance != nullptr ? _instance->_hiddenWiredPageCount : 0;
  }
};

#endif
