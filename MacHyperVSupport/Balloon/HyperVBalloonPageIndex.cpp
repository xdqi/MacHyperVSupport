//
//  HyperVBalloonPageIndex.cpp
//  Hyper-V balloon index of grabbed pages
//
//  Copyright © 2026 xdqi. All rights reserved.
//

#include <string.h>
#include <IOKit/IOLib.h>

#include "HyperVBalloonPageIndex.hpp"

//
// The bucket array starts with 1024 buckets and doubles once there are 4 pages per bucket, which takes
// 1 to 2 bytes per page on 32-bit kernels and 2 to 4 bytes per page on 64-bit kernels at most ballooned pages.
//
#define kHyperVBalloonPageIndexInitialBucketShift 10
#define kHyperVBalloonPageIndexMaxBucketShift     24
#define kHyperVBalloonPageIndexMaxLoadFactor      4U

UInt32 HyperVBalloonPageIndex::getBucketIndex(ppnum_t pageFrameNumber, UInt32 bucketShift) {
  //
  // Fibonacci hashing spreads runs of adjacent page frame numbers over all buckets.
  //
  return (UInt32) (pageFrameNumber * 0x9E3779B9U) >> (32 - bucketShift);
}

HyperVKernelPageRef *HyperVBalloonPageIndex::allocateBuckets(UInt32 bucketShift) {
  size_t              size    = sizeof(HyperVKernelPageRef) << bucketShift;
  HyperVKernelPageRef *buckets = static_cast<HyperVKernelPageRef *>(IOMalloc(size));
  if (buckets != nullptr) {
    memset(buckets, 0, size);
  }
  return buckets;
}

void HyperVBalloonPageIndex::freeBuckets() {
  IOFree(_buckets, sizeof(*_buckets) << _bucketShift);
  _buckets     = nullptr;
  _bucketShift = 0;
}

void HyperVBalloonPageIndex::grow() {
  //
  // Grow to the size the number of pages needs at once, the bucket array may have stayed small for many pages.
  //
  UInt32 newBucketShift = _bucketShift;
  while (_buckets != nullptr && newBucketShift < kHyperVBalloonPageIndexMaxBucketShift
         && _pageCount >= (kHyperVBalloonPageIndexMaxLoadFactor << newBucketShift)) {
    ++newBucketShift;
  }
  if (newBucketShift == _bucketShift) {
    return;
  }

  //
  // Allocating the bucket array waits for free pages, which may never come while ballooning under memory pressure.
  // Only grow with enough free pages, otherwise try again after some more pages. Longer chains only make lookups slower.
  //
  HyperVKernelPageRef *newBuckets = nullptr;
  if (HyperVBalloonKernelPages::hasFreePages((UInt32) ((sizeof(*newBuckets) << newBucketShift) >> PAGE_SHIFT))) {
    newBuckets = allocateBuckets(newBucketShift);
  }
  if (newBuckets == nullptr) {
    _growPageCount = _pageCount + _pageCount / 8;
    return;
  }

  for (UInt32 i = 0; i < (1U << _bucketShift); ++i) {
    HyperVKernelPageRef page = _buckets[i];
    while (page != nullptr) {
      HyperVKernelPageRef nextPage    = HyperVBalloonKernelPages::getLink(page);
      UInt32              bucketIndex = getBucketIndex(HyperVBalloonKernelPages::getPageFrameNumber(page), newBucketShift);
      HyperVBalloonKernelPages::setLink(page, newBuckets[bucketIndex]);
      newBuckets[bucketIndex] = page;
      page = nextPage;
    }
  }

  freeBuckets();
  _buckets       = newBuckets;
  _bucketShift   = newBucketShift;
  _growPageCount = kHyperVBalloonPageIndexMaxLoadFactor << _bucketShift;
}

bool HyperVBalloonPageIndex::insert(HyperVKernelPageRef page) {
  if (_buckets == nullptr) {
    _buckets = allocateBuckets(kHyperVBalloonPageIndexInitialBucketShift);
    if (_buckets == nullptr) {
      return false;
    }
    _bucketShift   = kHyperVBalloonPageIndexInitialBucketShift;
    _growPageCount = kHyperVBalloonPageIndexMaxLoadFactor << _bucketShift;
  } else if (_pageCount >= _growPageCount) {
    grow();
  }

  UInt32 bucketIndex = getBucketIndex(HyperVBalloonKernelPages::getPageFrameNumber(page), _bucketShift);
  HyperVBalloonKernelPages::setLink(page, _buckets[bucketIndex]);
  _buckets[bucketIndex] = page;
  ++_pageCount;
  return true;
}

HyperVKernelPageRef HyperVBalloonPageIndex::remove(UInt64 pageFrameNumber) {
  // Kernel page frame numbers are 32-bit, the host may send anything
  if (_buckets == nullptr || pageFrameNumber > 0xFFFFFFFFULL) {
    return nullptr;
  }

  UInt32              bucketIndex  = getBucketIndex((ppnum_t) pageFrameNumber, _bucketShift);
  HyperVKernelPageRef previousPage = nullptr;
  HyperVKernelPageRef page         = _buckets[bucketIndex];
  while (page != nullptr) {
    HyperVKernelPageRef nextPage = HyperVBalloonKernelPages::getLink(page);
    if (HyperVBalloonKernelPages::getPageFrameNumber(page) == pageFrameNumber) {
      if (previousPage == nullptr) {
        _buckets[bucketIndex] = nextPage;
      } else {
        HyperVBalloonKernelPages::setLink(previousPage, nextPage);
      }
      HyperVBalloonKernelPages::setLink(page, nullptr);

      --_pageCount;
      if (_pageCount == 0) {
        freeBuckets();
      }
      return page;
    }
    previousPage = page;
    page         = nextPage;
  }

  return nullptr;
}
