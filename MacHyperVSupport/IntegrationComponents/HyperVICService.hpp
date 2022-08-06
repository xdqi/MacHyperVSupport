//
//  HyperVICService.hpp
//  Hyper-V IC base class
//
//  Copyright © 2021 Goldfish64. All rights reserved.
//

#ifndef HyperVICService_hpp
#define HyperVICService_hpp

#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOService.h>

#include "HyperVVMBusDevice.hpp"
#include "HyperVIC.hpp"

#define kHyperVICBufferSize     4096

class HyperVICService : public IOService {
  OSDeclareDefaultStructors(HyperVICService);
  HVDeclareLogFunctionsVMBusChild("ic");
  typedef IOService super;

private:
  void freeStructures();
  void handleInterrupt(OSObject *owner, IOInterruptEventSource *sender, int count);
  IOInterruptEventSource *interruptSource = nullptr;
  
protected:
  HyperVVMBusDevice *hvDevice = nullptr;
  void setICDebug(bool debug) { debugEnabled = debug; }
  
  virtual bool processMessage() = 0;
  
  bool createNegotiationResponse(VMBusICMessageNegotiate *negMsg, UInt32 fwVersion, UInt32 msgVersion);
  
public:
  //
  // IOService overrides.
  //
  virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
  virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};

#endif
