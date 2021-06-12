#ifndef BUFFER_MANAGER_H
#define BUFFER_MANAGER_H

#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "errors.h"

typedef struct
{
    void * buffer;
    void * cursor;
    ssize_t len;
    ssize_t cap;
    int file;
} BufferRead;

Error init_ReadBuffer(BufferRead * buffer_read, int file, ssize_t size);
Error _read_at_least(BufferRead * buffer_read, ssize_t at_least);

/*
 *
 *  Force function inlining when there are enough bytes in the buffer (avoid unnecessary funciton calls)
 *
 */
static Error
read_at_least(BufferRead * buffer_read, ssize_t at_least)
{
    ssize_t bytes_left = buffer_read->len - (buffer_read->cursor - buffer_read->buffer);

    if (bytes_left < at_least)
        return _read_at_least(buffer_read, at_least);
    
    return SUCCESS;
}

#define BUFFER_READ(T) static Error                            \
    T##_from_BufferRead(BufferRead * buffer_read, T * var)     \
    {                                                          \
        Error error = read_at_least(buffer_read, sizeof (T));  \
        if (error == SUCCESS)                                  \
        {                                                      \
            T ** ptr_cursor = (T **) &buffer_read->cursor;     \
            *var = *(*ptr_cursor)++;                           \
        }                                                      \
                                                               \
        return error;                                          \
    }


typedef struct
{
    void * buffer;
    ssize_t len;
    ssize_t cap;
} BufferWrite;

static void
buffer_to_BufferWrite(BufferWrite * buffer_write, const void * const buffer, ssize_t size)
    {
        ssize_t len = buffer_write->len;
        assert(size <= (buffer_write->cap - len));
        memcpy(buffer_write->buffer + len, buffer, size);
        buffer_write->len += size;
    }


#define BUFFER_WRITE(T) static void                              \
    T##_to_BufferWrite(BufferWrite * buffer_write, T var)        \
    {                                                            \
        ssize_t len = buffer_write->len;                         \
        assert(sizeof (T) <= (buffer_write->cap - len));         \
        *((T *) (buffer_write->buffer + len)) = var;             \
        buffer_write->len += sizeof (T);                         \
    }

#define u32 uint32_t

BUFFER_READ(u32) BUFFER_WRITE(u32)

#undef BUFFER_READ
#undef BUFFER_WRITE

#endif // BUFFER_MANAGER_H