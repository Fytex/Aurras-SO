#include <stdlib.h>

#include "buffer_manager.h"
#include "errors.h"

#define PAGE_SIZE 4096


Error
init_ReadBuffer(BufferRead * buffer_read, int file, ssize_t size)
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
_read_at_least(BufferRead * buffer_read, ssize_t at_least)
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
        buffer_read->cursor = cursor = buffer;

    else if (bytes_left <= cursor - buffer) // Memory can't overlap with memcpy
    {
        memcpy(buffer, cursor, bytes_left);
        buffer_read->cursor = cursor = buffer;
    }
    else if (at_least == 0 || (cursor - buffer) + at_least > cap) // If it execeds the size of the buffer or we want any block. Then we get rid of the junk
    {
        memmove(buffer, cursor, bytes_left); // O(N) operation -> Heavy
        buffer_read->cursor = cursor = buffer;
    }


    if (at_least == 0) // If not specified then we give the first block we receive
        at_least = 1;

    printf("At LEast: %d\n", at_least);
    while (bytes_left < at_least)
    {
        bytes_read = read(file, buffer, cap - len);

        if (bytes_read == -1)
            return COMMUNICATION_FAILED;
        else if (bytes_read == 0)
            return NO_OPPOSITE_CONN;

        bytes_left += bytes_read;
        len += bytes_read;   
    }

    buffer_read->len = len;
    return SUCCESS;
}