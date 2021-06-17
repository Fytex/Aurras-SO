#include <stdlib.h>

#include "buffer_manager.h"
#include "errors.h"

int ALARM_INTERRUPT = 0;


Error
init_ReadBuffer(BufferRead * const buffer_read, int file, ssize_t size)
{
    void * buffer = malloc(size == 0 ? PAGE_SIZE : size);

    if (buffer == NULL)
        return NOT_ENOUGH_MEMORY;

    *buffer_read = (BufferRead)
    {
        .buffer = buffer,
        .cap = size,
        .cursor = buffer,
        .file = file,
        .len = 0
    };

    return SUCCESS;
}


/*
 *  ---- Internal Function ----
 *  This works as a clock buffer. If the final size is bigger than buffer + cap then it will copy all unused values to the beginning
 *  Buffer is always a multiple of PAGE_SIZE
 */
Error
_read_at_least(BufferRead * const buffer_read, ssize_t at_least)
{
    ssize_t bytes_read;
    void * buffer = buffer_read->buffer;
    void * cursor = buffer_read->cursor;
    ssize_t cap = buffer_read->cap;
    ssize_t len = buffer_read->len;
    int file = buffer_read->file;


    if (cap < at_least)
    {
        do
        {
            cap += PAGE_SIZE;
        }
        while (cap < at_least);

        void * new_buffer = realloc(buffer, cap);

        if (new_buffer == NULL)
            return NOT_ENOUGH_MEMORY;
        
        buffer_read->cursor = cursor = new_buffer + (cursor - buffer);
        buffer_read->buffer = buffer = new_buffer;
        buffer_read->cap = cap;
    }

    ssize_t bytes_left = len - (cursor - buffer);

    /*
     * Better to copy memory then do more calls to read from disk
     */

    if (bytes_left == 0)
    {
        buffer_read->cursor = cursor = buffer;
        buffer_read->len = len = 0;
    }

    else if (bytes_left <= cursor - buffer) // Memory can't overlap with memcpy
    {
        memcpy(buffer, cursor, bytes_left);
        buffer_read->cursor = cursor = buffer;
        buffer_read->len = len = bytes_left;
    }
    else if (at_least == 0 || (cursor - buffer) + at_least > cap) // If it execeds the size of the buffer or we want any block. Then we get rid of the junk
    {
        memmove(buffer, cursor, bytes_left); // O(N) operation -> Heavy
        buffer_read->cursor = cursor = buffer;
        buffer_read->len = len = bytes_left;
    }

    if (at_least == 0) // If not specified then we give the first block we receive
        at_least = 1;
    
    ALARM_INTERRUPT = 0;
    alarm(CLIENT_TIMEOUT_TIME); // Avoid waiting
    Error error = SUCCESS;

    while (error == SUCCESS && bytes_left < at_least)
    {
        bytes_read = read(file, buffer, cap - len);

        if (ALARM_INTERRUPT == 1)
        {
            error = CLIENT_TIMEOUT;
            if (bytes_read > 0)
                len += bytes_read; 
        }
    
        else if (bytes_read == -1)
            error = COMMUNICATION_FAILED;

        else if (bytes_read == 0)
            error = NO_OPPOSITE_CONN;

        else
        {
            bytes_left += bytes_read;
            len += bytes_read;  
        }

    }

    alarm(0); // reset alarm

    buffer_read->len = len;
    return error;
}


Error
init_WriteBuffer(BufferWrite * const buffer_write, int file, ssize_t size)
{
    if (size == 0)
        size = PAGE_SIZE;

    void * buffer = malloc(size);

    if (buffer == NULL)
        return NOT_ENOUGH_MEMORY;

    *buffer_write = (BufferWrite)
    {
        .buffer = buffer,
        .cap = size,
        .file = file,
        .len = 0
    };

    return SUCCESS;
}


Error
buffer_to_BufferWrite(BufferWrite * const buffer_write, const void * const buffer, ssize_t size)
{
    ssize_t len = buffer_write->len;

    if (len + size > buffer_write->cap)
    {
        Error error = flush_BufferWrite(buffer_write);
        if (error != SUCCESS)
            return error;
    }

    memcpy(buffer_write->buffer + len, buffer, size);
    buffer_write->len += size;
    return SUCCESS;
}