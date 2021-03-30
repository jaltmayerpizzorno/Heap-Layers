#pragma once
#ifndef HL_CALLSTACK_H
#define HL_CALLSTACK_H

/**
 * @class Callstack
 * @brief Saves the current call stack and formats it upon demand.
 * @author Juan Altmayer Pizzorno
 **/

#include <cxxabi.h>
#include <iostream>
#include <iomanip>

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

#if defined(USE_LIBBACKTRACE) && USE_LIBBACKTRACE
static backtrace_state* get_libbacktrace_state() {
  static backtrace_state* btstate = backtrace_create_state(nullptr, true, nullptr, nullptr);
  return btstate;
}
#endif

template <int MAX_FRAMES = 16>
class Callstack {
  int _nFrames{0};
  void* _frames[MAX_FRAMES];

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
  Callstack() {
    _nFrames = ::backtrace(_frames, MAX_FRAMES);
  }

  void print(std::ostream& out, const std::string& indent = "  ") const {
    for (int i=0; i<_nFrames; i++) { // XXX skip 1st because it's invariably our malloc() above?
      out << indent << std::setw(2+2*8) // "0x" + 64-bit ptr
                    << _frames[i];

      Dl_info info;
      if (!dladdr(_frames[i], &info)) {
        memset(&info, 0, sizeof(info));
      }

      if (info.dli_fname) {
        out << " [" << info.dli_fname << "]";
      }

      bool hasInfo{false};

      #if defined(USE_LIBBACKTRACE) && USE_LIBBACKTRACE
        struct Context {
          std::ostream& out;
          const std::string& indent;
          bool& hasInfo;
        } ctx{out, indent, hasInfo};

        backtrace_pcinfo(get_libbacktrace_state(),
                         (uintptr_t)_frames[i], [](void *data, uintptr_t pc,
                                                   const char *filename, int lineno,
                                                   const char *function) {
            Context* ctx = (Context*)data;

            if (ctx->hasInfo) {
              ctx->out << "\n" << ctx->indent << std::string(18, ' ') << ctx->indent;
            }

            if (function) {
              ctx->hasInfo = true;

              if (char* cppName = demangle(function)) {
                ctx->out << " " << cppName;
                cxa_free(cppName);
              }
              else {
                ctx->out << " " << function;
              }
            }

            if (filename != nullptr && lineno != 0) {
              ctx->hasInfo = true;
              ctx->out << " " << filename << ":" << lineno;
            } 

            return 0;   // alternatively, return 1 to stop at the 1st level
        }, [](void *data, const char *msg, int errnum) {
          // error ignored
          // Context* ctx = (Context*)data;
          // ctx->out << "(libbacktrace: " << msg << ")";
        }, &ctx);
      #endif
      if (!hasInfo && info.dli_sname != 0) {
        if (char* cppName = demangle(info.dli_sname)) {
          out << " " << cppName;
          cxa_free(cppName);
        }
        else {
          out << " " << info.dli_sname;
        }

        out << "+" << (uintptr_t)_frames[i] - (uintptr_t)info.dli_saddr;
      }

      out << "\n";
    }
  }
};

} // namespace

#endif
