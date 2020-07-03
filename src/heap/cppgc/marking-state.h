// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_CPPGC_MARKING_STATE_H_
#define V8_HEAP_CPPGC_MARKING_STATE_H_

#include "include/cppgc/trace-trait.h"
#include "src/heap/cppgc/globals.h"
#include "src/heap/cppgc/heap-object-header.h"
#include "src/heap/cppgc/heap-page.h"
#include "src/heap/cppgc/liveness-broker.h"
#include "src/heap/cppgc/marker.h"

namespace cppgc {
namespace internal {

// C++ marking implementation.
class MarkingState {
 public:
  inline MarkingState(HeapBase& heap, Marker::MarkingWorklist*,
                      Marker::NotFullyConstructedWorklist*,
                      Marker::WeakCallbackWorklist*, int);

  MarkingState(const MarkingState&) = delete;
  MarkingState& operator=(const MarkingState&) = delete;

  inline void MarkAndPush(const void*, TraceDescriptor);
  inline void MarkAndPush(HeapObjectHeader&, TraceDescriptor);
  inline void MarkAndPush(HeapObjectHeader&);

  inline bool MarkNoPush(HeapObjectHeader&);

  inline void DynamicallyMarkAddress(ConstAddress);

  inline void RegisterWeakReferenceIfNeeded(const void*, TraceDescriptor,
                                            WeakCallback, const void*);
  inline void RegisterWeakCallback(WeakCallback, const void*);
  inline void InvokeWeakRootsCallbackIfNeeded(const void*, TraceDescriptor,
                                              WeakCallback, const void*);

  inline void AccountMarkedBytes(const HeapObjectHeader&);
  size_t marked_bytes() const { return marked_bytes_; }

 private:
#ifdef DEBUG
  HeapBase& heap_;
#endif  // DEBUG

  Marker::MarkingWorklist::View marking_worklist_;
  Marker::NotFullyConstructedWorklist::View not_fully_constructed_worklist_;
  Marker::WeakCallbackWorklist::View weak_callback_worklist_;

  size_t marked_bytes_ = 0;
};

MarkingState::MarkingState(
    HeapBase& heap, Marker::MarkingWorklist* marking_worklist,
    Marker::NotFullyConstructedWorklist* not_fully_constructed_worklist,
    Marker::WeakCallbackWorklist* weak_callback_worklist, int task_id)
    :
#ifdef DEBUG
      heap_(heap),
#endif  // DEBUG
      marking_worklist_(marking_worklist, task_id),
      not_fully_constructed_worklist_(not_fully_constructed_worklist, task_id),
      weak_callback_worklist_(weak_callback_worklist, task_id) {
}

void MarkingState::MarkAndPush(const void* object, TraceDescriptor desc) {
  DCHECK_NOT_NULL(object);
  if (desc.base_object_payload ==
      cppgc::GarbageCollectedMixin::kNotFullyConstructedObject) {
    // This means that the objects are not-yet-fully-constructed. See comments
    // on GarbageCollectedMixin for how those objects are handled.
    not_fully_constructed_worklist_.Push(object);
    return;
  }
  MarkAndPush(HeapObjectHeader::FromPayload(
                  const_cast<void*>(desc.base_object_payload)),
              desc);
}

void MarkingState::MarkAndPush(HeapObjectHeader& header, TraceDescriptor desc) {
  DCHECK_NOT_NULL(desc.callback);

  if (header.IsInConstruction<HeapObjectHeader::AccessMode::kNonAtomic>()) {
    not_fully_constructed_worklist_.Push(header.Payload());
  } else if (MarkNoPush(header)) {
    marking_worklist_.Push(desc);
  }
}

bool MarkingState::MarkNoPush(HeapObjectHeader& header) {
  // A GC should only mark the objects that belong in its heap.
  DCHECK_EQ(&heap_, BasePage::FromPayload(&header)->heap());
  // Never mark free space objects. This would e.g. hint to marking a promptly
  // freed backing store.
  DCHECK(!header.IsFree());
  return header.TryMarkAtomic();
}

void MarkingState::DynamicallyMarkAddress(ConstAddress address) {
  HeapObjectHeader& header =
      BasePage::FromPayload(address)->ObjectHeaderFromInnerAddress(
          const_cast<Address>(address));
  DCHECK(!header.IsInConstruction<HeapObjectHeader::AccessMode::kNonAtomic>());
  if (MarkNoPush(header)) {
    marking_worklist_.Push(
        {reinterpret_cast<void*>(header.Payload()),
         GlobalGCInfoTable::GCInfoFromIndex(header.GetGCInfoIndex()).trace});
  }
}

void MarkingState::MarkAndPush(HeapObjectHeader& header) {
  MarkAndPush(
      header,
      {header.Payload(),
       GlobalGCInfoTable::GCInfoFromIndex(header.GetGCInfoIndex()).trace});
}

void MarkingState::RegisterWeakReferenceIfNeeded(const void* object,
                                                 TraceDescriptor desc,
                                                 WeakCallback weak_callback,
                                                 const void* parameter) {
  // Filter out already marked values. The write barrier for WeakMember
  // ensures that any newly set value after this point is kept alive and does
  // not require the callback.
  if (desc.base_object_payload !=
          cppgc::GarbageCollectedMixin::kNotFullyConstructedObject &&
      HeapObjectHeader::FromPayload(desc.base_object_payload)
          .IsMarked<HeapObjectHeader::AccessMode::kAtomic>())
    return;
  RegisterWeakCallback(weak_callback, parameter);
}

void MarkingState::InvokeWeakRootsCallbackIfNeeded(const void* object,
                                                   TraceDescriptor desc,
                                                   WeakCallback weak_callback,
                                                   const void* parameter) {
  if (desc.base_object_payload ==
      cppgc::GarbageCollectedMixin::kNotFullyConstructedObject) {
    // This method is only called at the end of marking. If the object is in
    // construction, then it should be reachable from the stack.
    return;
  }
  // Since weak roots are only traced at the end of marking, we can execute
  // the callback instead of registering it.
  weak_callback(LivenessBrokerFactory::Create(), parameter);
}

void MarkingState::RegisterWeakCallback(WeakCallback callback,
                                        const void* object) {
  weak_callback_worklist_.Push({callback, object});
}

void MarkingState::AccountMarkedBytes(const HeapObjectHeader& header) {
  marked_bytes_ +=
      header.IsLargeObject()
          ? reinterpret_cast<const LargePage*>(BasePage::FromPayload(&header))
                ->PayloadSize()
          : header.GetSize();
}

}  // namespace internal
}  // namespace cppgc

#endif  // V8_HEAP_CPPGC_MARKING_STATE_H_