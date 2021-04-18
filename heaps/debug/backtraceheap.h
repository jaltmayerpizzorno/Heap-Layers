// -*- C++ -*-

#pragma once
#ifndef HL_BACKTRACEHEAP_H
#define HL_BACKTRACEHEAP_H

/**
 * @class BacktraceHeap
 * @brief Saves the call stack for each allocation, displaying them on demand.
 * @author Juan Altmayer Pizzorno
 **/

#include "utility/callstack.h"

namespace HL {

  template <class SuperHeap, int MAX_FRAMES = 16>
  class BacktraceHeap : public SuperHeap {
   public:
    using CallstackType = Callstack<MAX_FRAMES>;

   private:
    struct alignas(SuperHeap::Alignment) TraceObj : public CallstackType {
      // FIXME use utility/dllist.h ?
      TraceObj* next;
      TraceObj* prev;
    };

    // This mutex is recursive because our malloc() and free() are invoked from within print_leaks()
    // if ::malloc() and ::free() point to this heap by way of LD_PRELOAD or some such mechanism.
    std::recursive_mutex _mutex;
    TraceObj* _objects{nullptr};

    void link(TraceObj* obj) {
      std::lock_guard<std::recursive_mutex> guard(_mutex);

      obj->prev = nullptr;
      obj->next = _objects;
      if (_objects) _objects->prev = obj;
      _objects = obj;
    }

    void unlink(TraceObj* obj) {
      std::lock_guard<std::recursive_mutex> guard(_mutex);

      if (_objects == obj) _objects = obj->next;
      if (obj->prev) obj->prev->next = obj->next;
      if (obj->next) obj->next->prev = obj->prev;
    }

  public:
    void* malloc(size_t sz) {
      TraceObj* obj = (TraceObj*) SuperHeap::malloc(sz + sizeof(TraceObj));
      if (obj == nullptr) return obj;

      // Note that Callstack::Callstack() may invoke malloc; this code assumes that if
      // malloc has been interposed and forwards to here, the interposed malloc
      // detects and avoids an infinite recursion.
      new (obj) TraceObj;
      link(obj);

      return obj + 1;
    }


    void free(void* ptr) {
      TraceObj* obj = (TraceObj*)ptr;
      --obj;

      unlink(obj);
      obj->~TraceObj();
      SuperHeap::free(obj);
    }


    size_t getSize(void *ptr) {
      return SuperHeap::getSize(((TraceObj*)ptr)-1) - sizeof(TraceObj);
    }


    void clear_leaks() {
      std::lock_guard<std::recursive_mutex> guard(_mutex);
      _objects = nullptr;
    }


    void print_leaks() {
      static const std::string indent = "  ";

      // Note that our malloc() and free() may be invoked recursively from here
      // if ::malloc() and ::free() point to this heap by way of LD_PRELOAD or
      // some such mechanism.
      std::lock_guard<std::recursive_mutex> guard(_mutex);

      bool any = false;

      for (auto obj = _objects; obj; obj = obj->next) {
        if (any) {
          std::cerr << "---\n";
        }
        std::cerr << SuperHeap::getSize(obj) - sizeof(TraceObj) << " byte(s) leaked @ " << obj+1 << "\n";

        obj->print(std::cerr, indent);
        any = true;
      }
    }


    template<class CALLBACK>
    void observe_leaks(CALLBACK cb) {
      std::lock_guard<std::recursive_mutex> guard(_mutex);

      for (auto obj = _objects; obj; obj = obj->next) {
        cb(obj+1, SuperHeap::getSize(obj) - sizeof(TraceObj), static_cast<CallstackType*>(obj));
      }
    }
  };
}

#endif
