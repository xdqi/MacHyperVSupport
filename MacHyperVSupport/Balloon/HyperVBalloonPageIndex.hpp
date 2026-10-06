//
//  HyperVBalloonPageIndex.hpp
//  Hyper-V balloon index of grabbed pages
//
//  Copyright © 2026 xdqi. All rights reserved.
//

#ifndef HyperVBalloonPageIndex_hpp
#define HyperVBalloonPageIndex_hpp

#include "HyperVBalloonKernelPages.hpp"

//
// Finds grabbed pages by their page frame number, which is how the host returns ballooned memory.
//
// A hash table chained through the pages themselves, see HyperVBalloonKernelPages::setLink(),
// so only the bucket array is allocated. It grows with the number of pages and is freed once empty.
// Not thread safe, the balloon only uses it on the work loop of its VMBus device. Only the page count
// is read from elsewhere.
//
class HyperVBalloonPageIndex {
private:
  HyperVKernelPageRef *_buckets      = nullptr;
  UInt32              _bucketShift   = 0;
  UInt32              _pageCount     = 0;
  // Page count at which growing the bucket array is tried next
  UInt32              _growPageCount = 0;

  static UInt32 getBucketIndex(ppnum_t pageFrameNumber, UInt32 bucketShift);
  static HyperVKernelPageRef *allocateBuckets(UInt32 bucketShift);
  void freeBuckets();
  void grow();

public:
  //
  // Adds a grabbed page, returns false if there is no memory for the bucket array.
  //
  bool insert(HyperVKernelPageRef page);

  //
  // Removes and returns the page with the page frame number, nullptr if there is none.
  //
  HyperVKernelPageRef remove(UInt64 pageFrameNumber);

  UInt32 getCount() const {
    return _pageCount;
  }
};

#endif
