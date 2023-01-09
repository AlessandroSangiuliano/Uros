/*
 * Created on Wed Jan 04 2023
 *
 * Copyright (c) 2023 Your Alex Sangiuliano
 */

#include <stdio.h>
#include <stdarg.h>

int 
printf(const char *format, ...)
{
    va_list args;
    va_start(args,format);

    vprintf(format, args);

    va_end(args);

    return 0;
}
