//===--- BridgeNSString.mm - String <-> NSString Bridging -----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This implements runtime support for bridging between Swift and Objective-C
// types in cases where they aren't trivial.
//
//===----------------------------------------------------------------------===//

#include <Foundation/Foundation.h>
#include <objc/runtime.h>
#include "swift/Runtime/Alloc.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/ObjCBridge.h"

using namespace swift;

struct SwiftString;

extern "C" {

int64_t
_TSS4sizefRSSFT_Si(void *swiftString);

uint32_t
_TSS9subscriptFT3idxSi_Scg(uint64_t idx, void *swiftString);

void
swift_NSStringToString(NSString *nsstring, SwiftString *string);

NSString *
swift_StringToNSString(SwiftString *string);

}; // extern "C"

struct SwiftString {
  const char *base;
  size_t len;
  HeapObject *owner;
};

struct _NSSwiftString_s {
  Class isa;
  SwiftString swiftString;
};

@interface _NSSwiftString : NSString {
@public
  SwiftString swiftString;
}
- (unichar)characterAtIndex: (NSUInteger)index;
- (NSUInteger)length;
@end

@implementation _NSSwiftString
- (unichar)characterAtIndex: (NSUInteger)idx {
  static_assert(sizeof(unichar) == 2, "NSString is no longer UTF16?");
  // XXX FIXME
  // Become bug-for-bug compatible with NSString being UTF16.
  // In practice, this API is oblivious to UTF16 surrogate pairs.
  return _TSS9subscriptFT3idxSi_Scg(idx, &swiftString);
}

- (NSUInteger)length {
  return _TSS4sizefRSSFT_Si(&swiftString);
}

// Disable the warning about chaining dealloc to super, we *specifically* don't
// want to do that here.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-missing-super-calls"
- (void)dealloc {
  swift_release(swiftString.owner);
  objc_destructInstance(self); // fixup weak references
  swift_rawDealloc(self, 4);
}
@end
#pragma clang diagnostic pop

// FIXME: This causes static constructors, which isn't awesome.  It would be
// spiffier to use ifunc's if possible.
static const Class stringClasses[] = {
  [_NSSwiftString self],
  objc_lookUpClass("__NSCFConstantString"),
  objc_lookUpClass("__NSCFString"),
};

/// Helper function to grow the allocated heap object for
/// swift_NSStringToString in the case where the initial allocation is
/// insufficient.
static BoxPair _swift_NSStringToString_realloc(BoxPair oldBox,
                                               size_t oldBufSize,
                                               size_t newBufSize) {
  // Allocate the new box.
  BoxPair newBox = swift_allocPOD(newBufSize, alignof(void*) - 1);

  // Copy the data from the old box.
  memcpy(newBox.value, oldBox.value, oldBufSize);
  
  // Deallocate the old box. We know the box is POD and hasn't escaped, so we
  // can use the swift_deallocPOD fast path.
  swift_deallocPOD(oldBox.heapObject);
  
  return newBox;
}

/// Convert an NSString to a Swift String in the worst case, where we have to
/// use -[NSString getBytes:...:] to reencode the string value.
__attribute__((noinline,used))
static void
_swift_NSStringToString_slow(NSString *nsstring, SwiftString *string) {
  size_t len = [nsstring length];
  size_t bufSize = len * 2 + 1;
  
  // Allocate a POD heap object to hold the data.
  BoxPair box = swift_allocPOD(bufSize, alignof(void*) - 1);
  char *buf = reinterpret_cast<char *>(box.value);
  char *p = buf;

  NSRange rangeToEncode = NSMakeRange(0, len);
  
  if (rangeToEncode.length != 0) {
    size_t pSize = bufSize - 1;
    for (;;) {
      NSUInteger usedLength = 0;
      // Copy the encoded string to our buffer.
      BOOL ok = [nsstring getBytes:p
                         maxLength:pSize
                        usedLength:&usedLength
                          encoding:NSUTF8StringEncoding
                           options:0
                             range:rangeToEncode
                    remainingRange:&rangeToEncode];
      // The operation should have encoded some bytes.
      if (!ok)
        __builtin_trap();

      p += usedLength;
      
      // If we encoded the entire string, we're done.
      if (rangeToEncode.length == 0)
        break;
      
      // Otherwise, grow the buffer and try again.
      size_t newBufSize = bufSize + pSize;
      box = _swift_NSStringToString_realloc(box, bufSize, newBufSize);
      bufSize = newBufSize;
      buf = reinterpret_cast<char *>(box.value);
    }
  }
  
  // getBytes:...: doesn't add a null terminator.
  *p = '\0';

  string->base  = buf;
  string->len   = p - buf;
  string->owner = box.heapObject;
}

void
swift_NSStringToString(NSString *nsstring, SwiftString *string) {
  auto boxedString = reinterpret_cast<_NSSwiftString_s *>(nsstring);
  if (boxedString->isa == stringClasses[0]) {
    string->base  = boxedString->swiftString.base;
    string->len   = boxedString->swiftString.len;
    string->owner = boxedString->swiftString.owner;
    _swift_retain(string->owner);
    return;
  } else if (*(Class *)nsstring == stringClasses[1]) {
    // constant string
    string->base  = ((char **)nsstring)[2];
    string->len   = ((size_t *)nsstring)[3];
    string->owner = NULL;
    return;
  }
  _swift_NSStringToString_slow(nsstring, string);
}


NSString *
swift_StringToNSString(SwiftString *string) {
  // sizeof(_NSSwiftString) is not allowed
  auto r = static_cast<_NSSwiftString *>(swift_rawAlloc(4));
  *((Class *)r) = stringClasses[0];
  r->swiftString = *string;
  _swift_retain(r->swiftString.owner);
  return r;
}

/// (String, UnsafePointer<BOOL>) -> () block shim
using block_type = void (^)(id, BOOL*);
using code_type = void (*)(const char*, size_t, HeapObject*, BOOL*, HeapObject*);

extern "C"
block_type _TTbbTSSGVSs13UnsafePointerV10ObjectiveC8ObjCBool__T_(
                                                    code_type code,
                                                    HeapObject *data) {
  SwiftRAII dataRAII(data, true);
  return Block_copy(^void (id a, BOOL *b) {
    [a retain];
    SwiftString s;
    swift_NSStringToString((NSString*)a, &s);
    return code(s.base, s.len, s.owner, b, swift_retain(*dataRAII));
  });
}

