#include "PrintfStdOutExtension.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static stdOutHandler _printfStdOutHandlerPtr = 0;
static void* _printfStdOutHandlerUserPtr = 0;

static int redirected_vprintf(const char* tag, const char* format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
#ifdef _MSC_VER
    int length = _vscprintf(format, copy);
#else
    int length = vsnprintf(NULL, 0, format, copy);
#endif
    va_end(copy);
    if (length < 0)
        return length;

    const size_t prefixLength = tag ? strlen(tag) + 2 : 0;
    const size_t capacity = prefixLength + (size_t)length + 1;
    char localText[512];
    char* text = capacity <= sizeof(localText) ? localText : (char*)malloc(capacity);
    if (!text)
        return -1;
    if (tag)
    {
        memcpy(text, tag, prefixLength - 2);
        text[prefixLength - 2] = ':';
        text[prefixLength - 1] = ' ';
    }
    int result = vsnprintf(text + prefixLength, capacity - prefixLength, format, args);
    if (result >= 0 && _printfStdOutHandlerPtr)
        _printfStdOutHandlerPtr(text, _printfStdOutHandlerUserPtr);
    if (text != localText)
        free(text);
    return result;
}

int redirected_printf(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = redirected_vprintf(NULL, format, args);
    va_end(args);
    return result;
}

void __android_log_print(int severity, const char* tag, const char* format, ...)
{
    (void)severity;
    va_list args;
    va_start(args, format);
    redirected_vprintf(tag, format, args);
    va_end(args);
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
