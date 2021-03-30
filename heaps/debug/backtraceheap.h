// -*- C++ -*-

#pragma once
#ifndef HL_BACKTRACEHEAP_H
#define HL_BACKTRACEHEAP_H

/**
 * @class BacktraceHeap
 * @brief Saves the call stack for each allocation, displaying them on demand.
 * @author Juan Altmayer Pizzorno
 **/

#include <cxxabi.h>
#include <iostream>
#include <iomanip>
#include <atomic>


#if defined(USE_LIBBACKTRACE) && USE_LIBBACKTRACE
  #include "backtrace.h"    // // https://github.com/ianlancetaylor/libbacktrace
#endif

#if defined(__linux__) || defined(__APPLE__)
  #include <dlfcn.h>
  #include <execinfo.h>
#else
  #error "needs port"
#endif

namespace HL {

  template <class SuperHeap, int stackSize = 16>
  class BacktraceHeap : public SuperHeap {
    struct alignas(SuperHeap::Alignment) TraceObj {
      int nFrames{0};
      void* callStack[stackSize];
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

    static void cxa_free(char* demangled) {
      // On Linux, even using LD_PRELOAD wrapping we could just use ::free here,
      // but on MacOS, if malloc and free are interposed, "::free" still points
      // to the original library code, whereas __cxa_demangle will see (and use)
      // the interposed.  We need thus to look for the free wrapper, if any.
      static decltype(::free)* freep = (decltype(::free)*) dlsym(RTLD_DEFAULT, "free");
      (*freep)(demangled);
    }

    static char* demangle(const char* function) {
      int demangleStatus = 0;
      char* cppName = abi::__cxa_demangle(function, 0, 0, &demangleStatus);

      if (demangleStatus == 0) {
        return cppName;
      }

      cxa_free(cppName);
      return 0;
    }

  public:
    void* malloc(size_t sz) {
      TraceObj* obj = (TraceObj*) SuperHeap::malloc(sz + sizeof(TraceObj));
      if (obj == nullptr) return obj;

      // Note that backtrace() may invoke malloc; this code assumes that if
      // malloc has been interposed and forwards to here, the interposed malloc
      // detects and avoids an infinite recursion.
      obj->nFrames = backtrace(obj->callStack, stackSize);
      link(obj);

      return obj + 1;
    }


    void free(void* ptr) {
      TraceObj* obj = (TraceObj*)ptr;
      --obj;

      unlink(obj);
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

      #if defined(USE_LIBBACKTRACE) && USE_LIBBACKTRACE
        static backtrace_state* btstate{nullptr};
        if (btstate == nullptr) {
          btstate = backtrace_create_state(nullptr, true, nullptr, nullptr);
        }
      #endif

      bool any = false;

      for (auto obj = _objects; obj; obj = obj->next) {
        if (any) {
          std::cerr << "---\n";
        }
        std::cerr << SuperHeap::getSize(obj) - sizeof(TraceObj) << " byte(s) leaked @ " << obj+1 << "\n";

        for (int i=0; i<obj->nFrames; i++) { // XXX skip 1st because it's invariably our malloc() above?
          std::cerr << indent << std::setw(2+2*8) // "0x" + 64-bit ptr
                              << obj->callStack[i];

          Dl_info info;
          if (!dladdr(obj->callStack[i], &info)) {
            memset(&info, 0, sizeof(info));
          }

          if (info.dli_fname) {
            std::cerr << " [" << info.dli_fname << "]";
          }

          bool hasInfo{false};

          #if defined(USE_LIBBACKTRACE) && USE_LIBBACKTRACE
            backtrace_pcinfo(btstate, (uintptr_t)obj->callStack[i], [](void *data, uintptr_t pc,
                                                                       const char *filename, int lineno,
                                                                       const char *function) {
                    bool* hasInfo = (bool*)data;

                    if (*hasInfo) {
                      std::cerr << "\n" << indent << std::string(18, ' ') << indent;
                    }

                    if (function) {
                      *hasInfo = true;

                      if (char* cppName = demangle(function)) {
                        std::cerr << " " << cppName;
                        cxa_free(cppName);
                      }
                      else {
                        std::cerr << " " << function;
                      }
                    }

                    if (filename != nullptr && lineno != 0) {
                      *hasInfo = true;
                      std::cerr << " " << filename << ":" << lineno;
                    } 

                    return 0;   // alternatively, return 1 to stop at the 1st level
                }, [](void *data, const char *msg, int errnum) {
                  // error ignored
                  // std::cerr << "(libbacktrace: " << msg << ")";
                }, &hasInfo);
          #endif
          if (!hasInfo && info.dli_sname != 0) {
            if (char* cppName = demangle(info.dli_sname)) {
              std::cerr << " " << cppName;
              cxa_free(cppName);
            }
            else {
              std::cerr << " " << info.dli_sname;
            }

            std::cerr << "+" << (uintptr_t)obj->callStack[i] - (uintptr_t)info.dli_saddr;
          }

          std::cerr << "\n";
        }
        any = true;
      }
    }
  };
}

#endif
