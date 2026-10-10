#ifndef PRINTFSTDOUTEXTENSION_H
#define PRINTFSTDOUTEXTENSION_H

#if defined(_MSC_VER) && defined(LIBJAMESDSP_PLUGIN)
// Parse the CRT inline printf definition before redirecting DSP call sites.
#include <stdio.h>
int redirected_printf(const char* format, ...);
#define printf redirected_printf
#endif

typedef void (*stdOutHandler)(const char*, void*);


extern void setPrintfStdOutHandler(stdOutHandler funcPtr, void* userData);
extern int isPrintfStdOutHandlerSet();
extern void __android_log_print(int severity, const char* tag, const char* msg, ...)
#if defined(__GNUC__)
    __attribute__ ((format(printf, 3, 4)))
#endif
    ;

#endif // PRINTFSTDOUTEXTENSION_H
