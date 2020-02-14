/*
 * Copyright (c) 2016, 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "jfr/jfr.hpp"
#include "jfr/leakprofiler/leakProfiler.hpp"
#include "jfr/leakprofiler/checkpoint/objectSampleCheckpoint.hpp"
#include "jfr/metadata/jfrSerializer.hpp"
#include "jfr/recorder/checkpoint/jfrCheckpointWriter.hpp"
#include "jfr/recorder/checkpoint/types/jfrType.hpp"
#include "jfr/recorder/checkpoint/types/jfrTypeManager.hpp"
#include "jfr/utilities/jfrDoublyLinkedList.hpp"
#include "jfr/utilities/jfrIterator.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/thread.inline.hpp"
#include "utilities/exceptions.hpp"
#include "runtime/semaphore.hpp"

class JfrSerializerRegistration : public JfrCHeapObj {
 private:
  JfrSerializerRegistration* _next;
  JfrSerializerRegistration* _prev;
  JfrSerializer* _serializer;
  mutable JfrBlobHandle _cache;
  JfrTypeId _id;
  bool _permit_cache;

 public:
  JfrSerializerRegistration(JfrTypeId id, bool permit_cache, JfrSerializer* serializer) :
    _next(NULL), _prev(NULL), _serializer(serializer), _cache(), _id(id), _permit_cache(permit_cache) {}

  ~JfrSerializerRegistration() {
    delete _serializer;
  }

  JfrSerializerRegistration* next() const {
    return _next;
  }

  void set_next(JfrSerializerRegistration* next) {
    _next = next;
  }

  JfrSerializerRegistration* prev() const {
    return _prev;
  }

  void set_prev(JfrSerializerRegistration* prev) {
    _prev = prev;
  }

  JfrTypeId id() const {
    return _id;
  }

  void invoke(JfrCheckpointWriter& writer) const;
};

void JfrSerializerRegistration::invoke(JfrCheckpointWriter& writer) const {
  if (_cache.valid()) {
    writer.increment();
    _cache->write(writer);
    return;
  }
  const JfrCheckpointContext ctx = writer.context();
  // serialize the type id before invoking callback
  writer.write_type(_id);
  const intptr_t start = writer.current_offset();
  // invoke the serializer routine
  _serializer->serialize(writer);
  if (start == writer.current_offset() ) {
    // the serializer implementation did nothing, rewind to restore
    writer.set_context(ctx);
    return;
  }
  if (_permit_cache) {
    _cache = writer.copy(&ctx);
  }
}

class SerializerRegistrationGuard : public StackObj {
 private:
  static Semaphore _mutex_semaphore;
 public:
  SerializerRegistrationGuard() {
    _mutex_semaphore.wait();
  }
  ~SerializerRegistrationGuard() {
    _mutex_semaphore.signal();
  }
};

Semaphore SerializerRegistrationGuard::_mutex_semaphore(1);

typedef JfrDoublyLinkedList<JfrSerializerRegistration> List;
typedef StopOnNullIterator<const List> Iterator;
static List types;
static List safepoint_types;

void JfrTypeManager::clear() {
  SerializerRegistrationGuard guard;
  Iterator iter(types);
  JfrSerializerRegistration* registration;
  while (iter.has_next()) {
    registration = types.remove(iter.next());
    assert(registration != NULL, "invariant");
    delete registration;
  }
  Iterator sp_type_iter(safepoint_types);
  while (sp_type_iter.has_next()) {
    registration = safepoint_types.remove(sp_type_iter.next());
    assert(registration != NULL, "invariant");
    delete registration;
  }
}

void JfrTypeManager::write_types(JfrCheckpointWriter& writer) {
  const Iterator iter(types);
  while (iter.has_next()) {
    iter.next()->invoke(writer);
  }
}

void JfrTypeManager::write_safepoint_types(JfrCheckpointWriter& writer) {
  assert(SafepointSynchronize::is_at_safepoint(), "invariant");
  const Iterator iter(safepoint_types);
  while (iter.has_next()) {
    iter.next()->invoke(writer);
  }
}

void JfrTypeManager::write_type_set() {
  assert(!SafepointSynchronize::is_at_safepoint(), "invariant");
  // can safepoint here
  MutexLocker module_lock(PackageTable_lock);
  if (!LeakProfiler::is_running()) {
    JfrCheckpointWriter writer(true, true, Thread::current());
    TypeSet set;
    set.serialize(writer);
    return;
  }
  JfrCheckpointWriter leakp_writer(false, true, Thread::current());
  JfrCheckpointWriter writer(false, true, Thread::current());
  TypeSet set(&leakp_writer);
  set.serialize(writer);
  ObjectSampleCheckpoint::on_type_set(leakp_writer);
}

void JfrTypeManager::write_type_set_for_unloaded_classes() {
  assert(SafepointSynchronize::is_at_safepoint(), "invariant");
  JfrCheckpointWriter writer(false, true, Thread::current());
  const JfrCheckpointContext ctx = writer.context();
  ClassUnloadTypeSet class_unload_set;
  class_unload_set.serialize(writer);
  if (LeakProfiler::is_running()) {
    ObjectSampleCheckpoint::on_type_set_unload(writer);
  }
  if (!Jfr::is_recording()) {
    // discard anything written
    writer.set_context(ctx);
  }
}

void JfrTypeManager::create_thread_blob(JavaThread* jt) {
  assert(jt != NULL, "invariant");
  ResourceMark rm(jt);
  HandleMark hm(jt);
  JfrThreadConstant type_thread(jt);
  JfrCheckpointWriter writer(false, true, jt);
  writer.write_type(TYPE_THREAD);
  type_thread.serialize(writer);
  // create and install a checkpoint blob
  jt->jfr_thread_local()->set_thread_blob(writer.move());
  assert(jt->jfr_thread_local()->has_thread_blob(), "invariant");
}

void JfrTypeManager::write_thread_checkpoint(JavaThread* jt) {
  assert(jt != NULL, "JavaThread is NULL!");
  ResourceMark rm(jt);
  HandleMark hm(jt);
  JfrThreadConstant type_thread(jt);
  JfrCheckpointWriter writer(false, true, jt);
  writer.write_type(TYPE_THREAD);
  type_thread.serialize(writer);
}

#ifdef ASSERT
static void assert_not_registered_twice(JfrTypeId id, List& list) {
  const Iterator iter(list);
  while (iter.has_next()) {
    assert(iter.next()->id() != id, "invariant");
  }
}
#endif

static bool register_type(JfrTypeId id, bool require_safepoint, bool permit_cache, JfrSerializer* serializer) {
  assert(serializer != NULL, "invariant");
  JfrSerializerRegistration* const registration = new JfrSerializerRegistration(id, permit_cache, serializer);
  if (registration == NULL) {
    delete serializer;
    return false;
  }
  if (require_safepoint) {
    assert(!safepoint_types.in_list(registration), "invariant");
    DEBUG_ONLY(assert_not_registered_twice(id, safepoint_types);)
    safepoint_types.prepend(registration);
  } else {
    assert(!types.in_list(registration), "invariant");
    DEBUG_ONLY(assert_not_registered_twice(id, types);)
    types.prepend(registration);
  }
  return true;
}

bool JfrTypeManager::initialize() {
  SerializerRegistrationGuard guard;

  // register non-safepointing type serialization
  register_type(TYPE_FLAGVALUEORIGIN, false, true, new FlagValueOriginConstant());
  register_type(TYPE_INFLATECAUSE, false, true, new MonitorInflateCauseConstant());
  register_type(TYPE_GCCAUSE, false, true, new GCCauseConstant());
  register_type(TYPE_GCNAME, false, true, new GCNameConstant());
  register_type(TYPE_GCWHEN, false, true, new GCWhenConstant());
  register_type(TYPE_G1HEAPREGIONTYPE, false, true, new G1HeapRegionTypeConstant());
  register_type(TYPE_GCTHRESHOLDUPDATER, false, true, new GCThresholdUpdaterConstant());
  register_type(TYPE_METADATATYPE, false, true, new MetadataTypeConstant());
  register_type(TYPE_METASPACEOBJECTTYPE, false, true, new MetaspaceObjectTypeConstant());
  register_type(TYPE_G1YCTYPE, false, true, new G1YCTypeConstant());
  register_type(TYPE_REFERENCETYPE, false, true, new ReferenceTypeConstant());
  register_type(TYPE_NARROWOOPMODE, false, true, new NarrowOopModeConstant());
  register_type(TYPE_COMPILERPHASETYPE, false, true, new CompilerPhaseTypeConstant());
  register_type(TYPE_CODEBLOBTYPE, false, true, new CodeBlobTypeConstant());
  register_type(TYPE_VMOPERATIONTYPE, false, true, new VMOperationTypeConstant());
  register_type(TYPE_THREADSTATE, false, true, new ThreadStateConstant());

  // register safepointing type serialization
  register_type(TYPE_THREADGROUP, true, false, new JfrThreadGroupConstant());
  register_type(TYPE_THREAD, true, false, new JfrThreadConstantSet());
  return true;
}

// implementation for the static registration function exposed in the JfrSerializer api
bool JfrSerializer::register_serializer(JfrTypeId id, bool require_safepoint, bool permit_cache, JfrSerializer* serializer) {
  SerializerRegistrationGuard guard;
  return register_type(id, require_safepoint, permit_cache, serializer);
}
