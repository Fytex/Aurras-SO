#ifndef BUFFER_MANAGER_H
#define BUFFER_MANAGER_H

#include <arpa/inet.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "errors.h"

#define CLIENT_TIMEOUT_TIME 10
#define PAGE_SIZE 4096

extern int ALARM_INTERRUPT;
extern int ACTIVE_ALARM;

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
 * All defined functions in `buffer_manager.h` are here for inlining purposes
 */



/*
 *
 *  Force function inlining when there are enough bytes in the buffer (avoid unnecessary funciton calls)
 *
 */
static inline Error
read_at_least(BufferRead * buffer_read, ssize_t at_least)
{
    ssize_t bytes_left = buffer_read->len - (buffer_read->cursor - buffer_read->buffer);

    if (bytes_left < at_least)
        return _read_at_least(buffer_read, at_least);
    
    return SUCCESS;
}


static inline void
reset_ReadBuffer_file(BufferRead * const buffer_read, const int file)
{
    buffer_read->len = 0;
    buffer_read->file = file;
    buffer_read->cursor = buffer_read->buffer;
}


#define BUFFER_READ(T, func) static inline Error               \
    T##_from_BufferRead(BufferRead * buffer_read, T * var)     \
    {                                                          \
        Error error = read_at_least(buffer_read, sizeof (T));  \
        if (error == SUCCESS)                                  \
        {                                                      \
            T ** ptr_cursor = (T **) &buffer_read->cursor;     \
            *var = func(*(*ptr_cursor)++);                     \
        }                                                      \
                                                               \
        return error;                                          \
    }

static inline void
free_ReadBuffer(BufferRead * buffer_read)
{
    free(buffer_read->buffer);
}


typedef struct
{
    void * buffer;
    ssize_t len;
    ssize_t cap;
    int file;
} BufferWrite;

Error init_WriteBuffer(BufferWrite * buffer_write, int file, ssize_t size);
Error buffer_to_BufferWrite(BufferWrite * buffer_write, const void * buffer, ssize_t size);

static inline Error
flush_BufferWrite(BufferWrite * buffer_write)
{
    if (buffer_write->len > 0)
    {
        if (write(buffer_write->file, buffer_write->buffer, buffer_write->len) == -1)
            return COMMUNICATION_FAILED;
        buffer_write->len = 0;
    }

    return SUCCESS;
}


#define BUFFER_WRITE(T, func) static inline Error                \
    T##_to_BufferWrite(BufferWrite * buffer_write, T var)        \
    {                                                            \
        if (buffer_write->len + sizeof (T) > buffer_write->cap)  \
        {                                                        \
            Error error = flush_BufferWrite(buffer_write);       \
            if (error != SUCCESS)                                \
                return error;                                    \
        }                                                        \
        ssize_t len = buffer_write->len;                         \
        *((T *) (buffer_write->buffer + len)) = func(var);       \
        buffer_write->len += sizeof (T);                         \
        return SUCCESS;                                          \
    }

static inline void
free_WriteBuffer(BufferWrite * buffer_write)
{
    free(buffer_write->buffer);
}

static inline Error
close_BufferWrite(BufferWrite * const buffer_write)
{
    Error error = flush_BufferWrite(buffer_write);
    free(buffer_write->buffer);
    close(buffer_write->file); // even with error it still force frees and closes

    return error;
}

#define u32 uint32_t
#define u8 uint8_t

BUFFER_READ(u32, ntohl) BUFFER_WRITE(u32, htonl)
BUFFER_READ(u8, )  BUFFER_WRITE(u8, )

#undef BUFFER_READ
#undef BUFFER_WRITE

#endif // BUFFER_MANAGER_H