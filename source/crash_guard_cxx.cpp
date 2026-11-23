// Uncaught C++ exception (dump 0035: vitaGL's GLSL preprocessor threw a
// std::string) -> report with the exception text instead of a bare abort().
#include <exception>
#include <string>
#include <psp2/kernel/clib.h>
#include "crash_guard.h"

static char what_buf[256];

static void on_terminate() {
    const char *what = "std::terminate() without an active exception";
    if (std::exception_ptr e = std::current_exception()) {
        what = "unknown exception type";
        try {
            std::rethrow_exception(e);
        } catch (const std::exception &x) {
            sceClibStrncpy(what_buf, x.what(), sizeof(what_buf) - 1);
            what = what_buf;
        } catch (const std::string &s) {
            sceClibStrncpy(what_buf, s.c_str(), sizeof(what_buf) - 1);
            what = what_buf;
        } catch (const char *s) {
            sceClibStrncpy(what_buf, s, sizeof(what_buf) - 1);
            what = what_buf;
        } catch (...) {
        }
    }
    crash_fatal("Uncaught C++ exception", what, 0, 0, nullptr, 0);
}

extern "C" void crash_guard_cxx_init(void) {
    std::set_terminate(on_terminate);
}
