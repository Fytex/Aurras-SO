#include "errors.h"

const char * error_msg(const int num)
{
    // Since this returns a const char * allocated at compile time typically placed in .rodata
    // So there won't be any Undefined Behaviour

    switch (num)
    {
        FOREACH_ERROR(ERROR_CASE)
    }

    return "Unknown error";
}