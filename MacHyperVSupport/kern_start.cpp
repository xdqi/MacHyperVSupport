//
//  kern_start.cpp
//  Hyper-V integration support Lilu configuration
//
//  Copyright © 2021 Goldfish64. All rights reserved.
//
// This kext is not a plugin, but requires an early start to
// properly register with Lilu's patcher functions.
//
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>

#include "HyperVPlatformProvider.hpp"
#include "HyperVBalloonAccounting.hpp"

PluginConfiguration ADDPR(config) {
  xStringify(PRODUCT_NAME),
  parseModuleVersion(xStringify(MODULE_VERSION)),
  LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
  NULL,
  0,
  NULL,
  0,
  NULL,
  0,
  KernelVersion::Tiger,
  KernelVersion::Tahoe,
  []() {
    HyperVPlatformProvider::getInstance();
    HyperVBalloonAccounting::getInstance();
  }
};
