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

#if __cplusplus >= 201703L
  #include <filesystem>
#endif

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

  static std::string normalize(const char* filepath) {
    #if __cplusplus >= 201703L
      namespace fs = std::filesystem;
      fs::path p = fs::path(filepath).lexically_normal();
      fs::path prox = p.lexically_proximate(fs::current_path());
      if (prox.string().compare(0, 2, "..") == 0) {
        return p;
      }
      return prox;
    #else
      return filepath;
    #endif
  }

public:
  Callstack() {}


  void backtrace() {
    _nFrames = ::backtrace(_frames, MAX_FRAMES);
  }


  int nFrames() const {
    return _nFrames;
  }


  void* frame(int i) const {
    assert(i < _nFrames);
    return _frames[i];
  }


  template<class CALLBACK>
  static void getpcinfo(void* pc, CALLBACK callback) {
    Dl_info info;
    if (!dladdr(pc, &info)) {
      memset(&info, 0, sizeof(info));
    }

    struct Context {
      decltype(callback)& cb;
      Dl_info& info;
      bool any{false};
    } ctx{callback, info};

    #if defined(USE_LIBBACKTRACE) && USE_LIBBACKTRACE
      backtrace_pcinfo(get_libbacktrace_state(),
                       (uintptr_t)pc, [](void *data, uintptr_t pc,
                                         const char *filename, int lineno,
                                         const char *function)->int {
        Context* ctx = (Context*)data;
        ctx->any = true;

        return ctx->cb(ctx->info.dli_fname, function, filename, lineno);

      }, [](void *data, const char *msg, int errnum) {
        // error ignored
      }, &ctx);
    #endif

    if (!ctx.any) {
      int offset = (uintptr_t)pc - (uintptr_t)info.dli_saddr;
      callback(info.dli_fname, info.dli_sname, nullptr, offset);
    }
  }


  void print(std::ostream& out, const std::string& indent = "  ") const {
    static const int ptrFieldWidth = 2+2*8; // "0x" + 64-bit ptr

    for (int i=0; i<_nFrames; i++) { // XXX skip 1st because it's invariably our malloc() above?
      out << indent << std::setw(ptrFieldWidth) << _frames[i];

      bool hasModule{false};
      bool anyOtherInfo{false};

      getpcinfo(_frames[i], [&](const char* module, const char* function,
                                const char* filename, int lineno)->bool {
          if (module && !hasModule) {
            out << " [" << normalize(module) << "]";
            hasModule = true;
          }

          if (anyOtherInfo) {
            out << "\n" << indent << std::string(ptrFieldWidth, ' ') << " ...";
          }

          if (function) {
            anyOtherInfo = true;

            if (char* cppName = demangle(function)) {
              out << " " << cppName;
              cxa_free(cppName);
            }
            else {
              out << " " << function;
            }
          }

          if (filename != nullptr) {
            anyOtherInfo = true;
            out << " " << normalize(filename) << ":" << lineno;
          } 
          else if (lineno) {
            out << "+" << lineno; // really offset
          }

          return false; // keep going
      });

      out << "\n";
    }
  }
};

} // namespace

#endif
