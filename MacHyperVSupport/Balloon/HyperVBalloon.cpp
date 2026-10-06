//
//  HyperVBalloon.cpp
//  Hyper-V balloon driver implementation
//
//  Copyright © 2022-2023 xdqi. All rights reserved.
//

#include "HyperVBalloon.hpp"
#include "HyperVBalloonAccounting.hpp"
#include "HyperVBalloonRegs.hpp"

OSDefineMetaClassAndStructors(HyperVBalloon, super);

bool HyperVBalloon::start(IOService *provider) {
  bool     result = false;
  IOReturn status;

  //
  // Get parent VMBus device object.
  //
  _hvDevice = OSDynamicCast(HyperVVMBusDevice, provider);
  if (_hvDevice == nullptr) {
    HVSYSLOG("Provider is not HyperVVMBusDevice");
    return false;
  }
  _hvDevice->retain();

  HVCheckDebugArgs();
  HVDBGLOG("Initializing Hyper-V Dynamic Memory");

  if (HVCheckOffArg()) {
    HVSYSLOG("Disabling Hyper-V Dynamic Memory due to boot arg");
    OSSafeReleaseNULL(_hvDevice);
    return false;
  }

  //
  // Status reports need the kernel memory statistics, which are resolved through Lilu.
  //
  if (!HyperVBalloonAccounting::canGetVMStatistics()) {
    HVSYSLOG("Disabling Hyper-V Dynamic Memory as kernel memory statistics are unavailable");
    OSSafeReleaseNULL(_hvDevice);
    return false;
  }

  if (!super::start(provider)) {
    HVSYSLOG("super::start() returned false");
    OSSafeReleaseNULL(_hvDevice);
    return false;
  }

  do {
    //
    // Allocate balloon state before the host can send any balloon request.
    //
    _chunks = static_cast<HyperVBalloonChunk **>(IOMalloc(sizeof(*_chunks) * kHyperVDynamicMemoryInitialChunkCapacity));
    if (_chunks != nullptr) {
      _chunkCapacity = kHyperVDynamicMemoryInitialChunkCapacity;
    }
    _balloonInflationSendBuffer = IOMalloc(kHyperVDynamicMemoryResponsePacketSize);
    if (_chunks == nullptr || _balloonInflationSendBuffer == nullptr) {
      HVSYSLOG("Failed to allocate balloon state");
      break;
    }

    //
    // Install packet handler.
    //
    status = _hvDevice->installPacketActions(this, OSMemberFunctionCast(HyperVVMBusDevice::PacketReadyAction, this, &HyperVBalloon::handlePacket), nullptr, kHyperVDynamicMemoryResponsePacketSize);
    if (status != kIOReturnSuccess) {
      HVSYSLOG("Failed to install packet handler with status 0x%X", status);
      break;
    }

    //
    // Open VMBus channel.
    //
    status = _hvDevice->openVMBusChannel(kHyperVDynamicMemoryBufferSize, kHyperVDynamicMemoryBufferSize);
    if (status != kIOReturnSuccess) {
      HVSYSLOG("Failed to open VMBus channel with status 0x%X", status);
      break;
    }

    //
    // Configure Hyper-V Dynamic Memory device.
    //
    if (!setupBalloon()) {
      HVSYSLOG("Unable to setup balloon device");
      break;
    }

    //
    // Initialize work loop and timer event source.
    //
    _workLoop = IOWorkLoop::workLoop();
    if (_workLoop == nullptr) {
      HVSYSLOG("Failed to initialize work loop");
      break;
    }

    _timerSource = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &HyperVBalloon::sendStatusReport));
    if (_timerSource == nullptr) {
      HVSYSLOG("Failed to initialize timed event source");
      break;
    }
    _workLoop->addEventSource(_timerSource);
    _timerSource->setTimeoutMS(kHyperVDynamicMemoryStatusReportIntervalMilliseconds);

    HVDBGLOG("Initialized Hyper-V Dynamic Memory");
    result = true;
  } while (false);

  if (!result) {
    stop(provider);
  }

  return result;
}

void HyperVBalloon::stop(IOService *provider) {
  HVDBGLOG("Hyper-V Dynamic Memory is stopping");

  //
  // Stop status reports before closing the channel they are sent on.
  //
  if (_timerSource != nullptr) {
    _timerSource->cancelTimeout();
    if (_workLoop != nullptr) {
      _workLoop->removeEventSource(_timerSource);
    }
    OSSafeReleaseNULL(_timerSource);
  }
  OSSafeReleaseNULL(_workLoop);

  if (_hvDevice != nullptr) {
    _hvDevice->closeVMBusChannel();
    _hvDevice->uninstallPacketActions();
    OSSafeReleaseNULL(_hvDevice);
  }

  if (_balloonInflationSendBuffer != nullptr) {
    IOFree(_balloonInflationSendBuffer, kHyperVDynamicMemoryResponsePacketSize);
    _balloonInflationSendBuffer = nullptr;
  }

  //
  // Pages still ballooned out are not backed by the host, freeing them would hand
  // unbacked memory back to the kernel. Leak them instead, like Linux does.
  //
  if (_chunkCount != 0) {
    HVSYSLOG("Leaking %u ballooned chunks", _chunkCount);
  } else if (_chunks != nullptr) {
    IOFree(_chunks, sizeof(*_chunks) * _chunkCapacity);
  }
  _chunks        = nullptr;
  _chunkCount    = 0;
  _chunkCapacity = 0;

  super::stop(provider);
}
