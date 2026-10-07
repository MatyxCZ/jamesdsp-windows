#include "PrintfStdOutExtension.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static stdOutHandler _printfStdOutHandlerPtr = 0;
static void* _printfStdOutHandlerUserPtr = 0;

int redirected_printf(const char* format, ...)
{
    va_list args;
    va_start(args, format);
#ifdef _MSC_VER
    va_list copy;
    va_copy(copy, args);
    int length = _vscprintf(format, copy);
    va_end(copy);
#else
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
#endif
    if (length < 0) {
        va_end(args);
        return length;
    }
    char* text = (char*)malloc((size_t)length + 1);
    if (!text) {
        va_end(args);
        return -1;
    }
    vsnprintf(text, (size_t)length + 1, format, args);
    va_end(args);
    if (_printfStdOutHandlerPtr)
        _printfStdOutHandlerPtr(text, _printfStdOutHandlerUserPtr);
    free(text);
    return length;
}

void __android_log_print(int severity, const char* tag, const char* msg)
{
    (void)severity;
    redirected_printf("%s: %s", tag, msg);
}

void setPrintfStdOutHandler(stdOutHandler funcPtr, void* userData)
{
    _printfStdOutHandlerPtr = funcPtr;
    _printfStdOutHandlerUserPtr = userData;
}

int isPrintfStdOutHandlerSet()
{
    return _printfStdOutHandlerPtr != 0;
}
