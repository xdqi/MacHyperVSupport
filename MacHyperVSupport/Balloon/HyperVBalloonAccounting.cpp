//
//  HyperVBalloonAccounting.cpp
//  Hyper-V balloon kernel memory accounting
//
//  Copyright © 2026 xdqi. All rights reserved.
//

#include "HyperVBalloonAccounting.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>

HyperVBalloonAccounting *HyperVBalloonAccounting::_instance;

//
// Leading fields of struct machine_info (osfmk/mach/machine.h), which is not part of the SDK.
// Older kernels have an avail_cpus field before memory_size, max_mem is verified before use.
//
typedef struct {
  integer_t major_version;
  integer_t minor_version;
  integer_t max_cpus;
  uint32_t  memory_size;
  uint64_t  max_mem;
} HyperVKernelMachineInfo;

//
// Leading fields of struct vm_locks_array (osfmk/vm/vm_page.h), which holds the page queue lock since 10.8.
//
typedef struct {
  char  pad             __attribute__((aligned(64)));
  UInt8 vmPageQueueLock __attribute__((aligned(64)));
} HyperVKernelVMLocksArray;
static_assert(offsetof(HyperVKernelVMLocksArray, vmPageQueueLock) == 64, "vm_page_queue_lock2 must be at offset 64");

void HyperVBalloonAccounting::init() {
  HVCheckDebugArgs();

  //
  // The page queue lock is a lck_mtx_t since 10.6.
  //
  if (getKernelVersion() < KernelVersion::SnowLeopard) {
    HVDBGLOG("Ballooned memory is not hidden on this kernel");
  } else if (checkKernelArgument("-hvballnohide")) {
    HVSYSLOG("Ballooned memory is not hidden due to boot arg");
  } else {
    _canHide = true;
  }

  lilu.onPatcherLoadForce([](void *user, KernelPatcher &patcher) {
    static_cast<HyperVBalloonAccounting *>(user)->onLiluPatcherLoad(patcher);
  }, this);
}

void HyperVBalloonAccounting::onLiluPatcherLoad(KernelPatcher &patcher) {
  auto solveSymbol = [&patcher](const char *symbol) {
    mach_vm_address_t address = patcher.solveSymbol(KernelPatcher::KernelID, symbol);
    patcher.clearError();
    return address;
  };

#if defined(__i386__)
  _hostStatistics = reinterpret_cast<HostStatisticsFunction>(solveSymbol("_host_statistics"));
#else
  _hostStatistics = reinterpret_cast<HostStatisticsFunction>(solveSymbol("_host_statistics64"));
#endif
  if (_hostStatistics == nullptr) {
    HVSYSLOG("Failed to resolve host statistics");
  }

  if (!_canHide) {
    return;
  }

  //
  // vm_page_queue_lock is a macro for a vm_page_locks field since 10.8.
  //
  HyperVKernelVMLocksArray *vmPageLocks = reinterpret_cast<HyperVKernelVMLocksArray *>(solveSymbol("_vm_page_locks"));
  if (vmPageLocks != nullptr) {
    _vmPageQueueLock = reinterpret_cast<lck_mtx_t *>(&vmPageLocks->vmPageQueueLock);
  } else {
    _vmPageQueueLock = reinterpret_cast<lck_mtx_t *>(solveSymbol("_vm_page_queue_lock"));
  }
  _vmPageWireCount = reinterpret_cast<unsigned int *>(solveSymbol("_vm_page_wire_count"));
  _maxMem          = reinterpret_cast<UInt64 *>(solveSymbol("_max_mem"));
  if (_vmPageQueueLock == nullptr || _vmPageWireCount == nullptr || _maxMem == nullptr) {
    HVSYSLOG("Failed to resolve kernel memory counters, ballooned memory is not hidden");
    return;
  }
  if (*_maxMem == 0 || (*_maxMem & PAGE_MASK) != 0 || *_vmPageWireCount >= (*_maxMem >> PAGE_SHIFT)) {
    HVSYSLOG("Unexpected memory size %llu with %u wired pages, ballooned memory is not hidden", *_maxMem, *_vmPageWireCount);
    return;
  }

  //
  // hw.memsize is max_mem_actual on macOS 11 and newer, and max_mem before.
  //
  _maxMemActual = reinterpret_cast<UInt64 *>(solveSymbol("_max_mem_actual"));
  if (_maxMemActual != nullptr && *_maxMemActual != *_maxMem) {
    HVSYSLOG("Actual memory size %llu differs from %llu, ballooned memory is not hidden", *_maxMemActual, *_maxMem);
    return;
  }

  //
  // Optional counters, the memory size reported by host_info() and the user wire limit.
  // The user wire limit is checked against the wired page count, so it is lowered by the same amount.
  //
  HyperVKernelMachineInfo *machineInfo = reinterpret_cast<HyperVKernelMachineInfo *>(solveSymbol("_machine_info"));
  if (machineInfo != nullptr && machineInfo->max_mem == *_maxMem) {
    _machineInfoMaxMem = &machineInfo->max_mem;
  } else {
    HVDBGLOG("Memory size in machine info not found");
  }
  _vmGlobalUserWireLimit = reinterpret_cast<UInt64 *>(solveSymbol("_vm_global_user_wire_limit"));
  if (_vmGlobalUserWireLimit != nullptr && *_vmGlobalUserWireLimit > *_maxMem) {
    HVDBGLOG("Unexpected user wire limit %llu", *_vmGlobalUserWireLimit);
    _vmGlobalUserWireLimit = nullptr;
  }

  _isEnabled = true;
  HVDBGLOG("Ballooned memory is hidden from kernel memory statistics (actual %u, machine info %u, wire limit %u)",
           _maxMemActual != nullptr, _machineInfoMaxMem != nullptr, _vmGlobalUserWireLimit != nullptr);
}

#if defined(__i386__)
kern_return_t HyperVBalloonAccounting::getVMStatistics(vm_statistics_t statistics, mach_msg_type_number_t *count) {
  if (!canGetVMStatistics()) {
    return KERN_FAILURE;
  }
  return _instance->_hostStatistics(host_priv_self(), HOST_VM_INFO, reinterpret_cast<host_info_t>(statistics), count);
}
#else
kern_return_t HyperVBalloonAccounting::getVMStatistics(vm_statistics64_t statistics, mach_msg_type_number_t *count) {
  if (!canGetVMStatistics()) {
    return KERN_FAILURE;
  }
  return _instance->_hostStatistics(host_priv_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(statistics), count);
}
#endif

bool HyperVBalloonAccounting::hide(UInt32 pageCount) {
  if (!_isEnabled) {
    return false;
  }

  UInt64 byteCount = (UInt64) pageCount << PAGE_SHIFT;
  bool   isHidden  = false;

  //
  // The kernel only changes the wired page count with the page queue lock held.
  // The pages are wired and thus already counted, the check only guards against kernel accounting changes.
  //
  lck_mtx_lock(_vmPageQueueLock);
  if (*_vmPageWireCount >= pageCount && *_maxMem > byteCount) {
    *_vmPageWireCount -= pageCount;
    *_maxMem          -= byteCount;
    if (_maxMemActual != nullptr) {
      *_maxMemActual -= byteCount;
    }
    if (_machineInfoMaxMem != nullptr) {
      *_machineInfoMaxMem -= byteCount;
    }
    if (_vmGlobalUserWireLimit != nullptr) {
      UInt64 wireLimitBytes = byteCount < *_vmGlobalUserWireLimit ? byteCount : *_vmGlobalUserWireLimit;
      *_vmGlobalUserWireLimit -= wireLimitBytes;
      _hiddenWireLimitBytes   += wireLimitBytes;
    }
    _hiddenPageCount += pageCount;
    isHidden = true;
  }
  lck_mtx_unlock(_vmPageQueueLock);

  if (!isHidden) {
    HVSYSLOG("Failed to hide %u pages", pageCount);
  }
  return isHidden;
}

void HyperVBalloonAccounting::unhide(UInt32 pageCount) {
  if (!_isEnabled) {
    return;
  }

  //
  // Nothing may be logged with the page queue lock held, logging can allocate memory.
  //
  lck_mtx_lock(_vmPageQueueLock);
  UInt32 hiddenPageCount   = _hiddenPageCount;
  UInt32 unhiddenPageCount = pageCount < hiddenPageCount ? pageCount : hiddenPageCount;

  UInt64 byteCount = (UInt64) unhiddenPageCount << PAGE_SHIFT;
  *_vmPageWireCount += unhiddenPageCount;
  *_maxMem          += byteCount;
  if (_maxMemActual != nullptr) {
    *_maxMemActual += byteCount;
  }
  if (_machineInfoMaxMem != nullptr) {
    *_machineInfoMaxMem += byteCount;
  }
  if (_vmGlobalUserWireLimit != nullptr) {
    UInt64 wireLimitBytes = byteCount < _hiddenWireLimitBytes ? byteCount : _hiddenWireLimitBytes;
    *_vmGlobalUserWireLimit += wireLimitBytes;
    _hiddenWireLimitBytes   -= wireLimitBytes;
  }
  _hiddenPageCount -= unhiddenPageCount;
  lck_mtx_unlock(_vmPageQueueLock);

  if (unhiddenPageCount < pageCount) {
    HVSYSLOG("Unhiding %u pages, but only %u pages were hidden", pageCount, hiddenPageCount);
  }
}
