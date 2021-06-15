#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>

#include "buffer_manager.h"
#include "errors.h"

#define MAIN_FIFO "tmp/FIFO_MAIN"
#define TMP_FOLDER "tmp/"
#define PROGRAM "aurras"

#define STRLEN(s) (sizeof(s)/sizeof(s[0])) - sizeof(s[0])

inline static void
print_cmds_help_to_stream(FILE * stream)
{
    fputs(
        "./" PROGRAM " status\n"
        "./" PROGRAM " transform input-filename output-filename filter-id-1 filter-id-2 ...\n",
        stream
    );
}

inline static void
print_cmds_help(void)
{
    print_cmds_help_to_stream(stdout);
}

static Error
create_connection(char ** const ext_fifo_str, uint32_t send_bytes)
{
    int main_fifo;
    ssize_t fifo_str_len;
    pid_t pid;
    char * fifo_str;
    Error error = SUCCESS;
    uint32_t data[2];

    if ((main_fifo = open(MAIN_FIFO, O_WRONLY)) != -1)
    {
        pid = getpid();
        fifo_str_len = STRLEN(TMP_FOLDER) + snprintf(NULL, 0, "%d", pid);
        fifo_str = malloc((fifo_str_len + 1) * sizeof (char));

        if (fifo_str != NULL)
        {

            snprintf(fifo_str, fifo_str_len, "%s%d", TMP_FOLDER, pid); // Using same function to garantee same code

            if (access(fifo_str, R_OK | W_OK) == 0 || mkfifo(fifo_str, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) != -1) // 0666
            {
                // The pid_t data type is a signed integer type which is capable of representing a process ID. In the GNU C Library, this is an int.
                // int in *nix at the moment are always 2 or 4 bytes
                data[0] = (int32_t) pid;
                data[1] = send_bytes;

                if (write(main_fifo, data, sizeof (data)) != -1)
                    *ext_fifo_str = fifo_str;
                else 
                {
                    free(fifo_str);
                    error = COMMUNICATION_FAILED;
                }

            }
            else 
            {
                free(fifo_str);
                error = CANT_CREATE_FIFO;
            }
        }
        else
            error = NOT_ENOUGH_MEMORY;
        
        close(main_fifo);
    }
    else
        error = MAIN_FIFO_FAIL;
   
    return error;
}



static Error
ask_status(void)
{
    Error error;
    BufferRead buffer_read;
    char * fifo_str;
    int fifo;
    pid_t server_pid;
    uint32_t n_tasks, n_filters, num_task;
    uint32_t running, max;
    ssize_t bytes_left, bytes_written;

    error = create_connection(&fifo_str, 0); // if we don't send any bytes then must be status command

    if (error != SUCCESS)
        return error;


    if ((fifo = open(fifo_str, O_RDONLY)) != -1)
    {
        error = init_ReadBuffer(&buffer_read, fifo, 0);
        if (error == SUCCESS)
        {
            if (u32_from_BufferRead(&buffer_read, (uint32_t *) &server_pid) == SUCCESS &&
                u32_from_BufferRead(&buffer_read, &n_tasks) == SUCCESS &&
                u32_from_BufferRead(&buffer_read, &n_filters) == SUCCESS)
            {

                for (int32_t i = 0; i < n_tasks; ++i)
                {
                    error = u32_from_BufferRead(&buffer_read, &num_task);

                    if (error != SUCCESS)
                        break;

                    printf("Task #%" PRId32 ": ", num_task);

                    do
                    {
                        error = read_at_least(&buffer_read, 0);
                        if (error != SUCCESS)
                            break;

                        bytes_left = buffer_read.len - (buffer_read.cursor - buffer_read.buffer);

                        /*
                            If last character is '\0' then bytes_left will stay with value 1
                        */
                        bytes_written = fprintf(stdout, "%.*s", (int) bytes_left, (char *) buffer_read.cursor);

                        buffer_read.cursor += bytes_written;
                    }
                    while (bytes_written == bytes_left);

                    if (error == SUCCESS)
                        ++buffer_read.cursor; // '\0' from string
                    else
                        break;

                    puts("");
                    
                }

                if (error == SUCCESS)
                {
                    for (int32_t i = 0; i < n_filters; ++i)
                    {
                        error = u32_from_BufferRead(&buffer_read, &running);
                        if (error != SUCCESS)
                            break;

                        error = u32_from_BufferRead(&buffer_read, &max);
                        if (error != SUCCESS)
                            break;


                        printf("Filter ");

                        do
                        {
                            error = read_at_least(&buffer_read, 0);
                            if (error != SUCCESS)
                                break;

                            bytes_left = buffer_read.len - (buffer_read.cursor - buffer_read.buffer);

                            /*
                                If last character is '\0' then bytes_left will stay with value 1
                            */
                            bytes_written = fprintf(stdout, "%.*s", (int) bytes_left, (char *) buffer_read.cursor);

                            buffer_read.cursor += bytes_written;
                        }
                        while (bytes_written == bytes_left);

                        if (error == SUCCESS)
                            ++buffer_read.cursor; // '\0' from string
                        else
                            break;
                        
                        printf(": %d/%d (running/max)\n", running, max);
                    }

                    if (error == SUCCESS)
                        printf("pid: %d", server_pid);
                }
            }
            free(buffer_read.buffer);
        }
        close(fifo);
    }
    
    free(fifo_str);
    return SUCCESS;

}


static Error
transform(const char * const input, const char * const output, const char * const * const filters, uint32_t n_filters)
{
    Error error;
    uint32_t send_bytes;
    uint32_t * filters_send_bytes;
    unsigned char * buffer, * buffer_cursor;
    char * fifo_str;
    int fifo;
    int8_t status;
    ssize_t bytes_read;
    int has_error;

    // No need to send an invalid message to the server since it has some costs...
    if (access(input, F_OK) == -1)
        return INPUT_NOT_FOUND;

    filters_send_bytes = malloc((n_filters + 2) * sizeof (size_t)); // filters + input + output

    if (filters_send_bytes == NULL)
        return NOT_ENOUGH_MEMORY;

    send_bytes = 0;

    send_bytes += (filters_send_bytes[0] = strlen(input) + 1);
    send_bytes += (filters_send_bytes[1] = strlen(output) + 1);

    for (int i = 0; i < n_filters; ++i)
        send_bytes += (filters_send_bytes[i + 2] = strlen(filters[i]) + 1);

    error = create_connection(&fifo_str, send_bytes);

    if (error == SUCCESS)
    {
    
        if ((fifo = open(fifo_str, O_WRONLY)) != -1) {

            buffer = malloc(send_bytes * sizeof (char));

            if (buffer != NULL)
            {
                buffer_cursor = buffer;

                // input
                memcpy(buffer_cursor, input, filters_send_bytes[0]);
                buffer_cursor += filters_send_bytes[0];

                // output
                memcpy(buffer_cursor, output, filters_send_bytes[1]);
                buffer_cursor += filters_send_bytes[1];

                for (int i = 0; i < n_filters; ++i)
                {
                    memcpy(buffer_cursor, filters[i], filters_send_bytes[i + 2]);
                    buffer_cursor += filters_send_bytes[i + 2];
                }

                if (write(fifo, buffer, send_bytes) == -1)
                {
                    free(buffer);
                    error = COMMUNICATION_FAILED;
                }

            }
            else
                error = NOT_ENOUGH_MEMORY;


            close(fifo);
        }
        else
            error = COMMUNICATION_FAILED;


        if (error == SUCCESS)
        {
            /*
             *  If reaches here and fifo still has the content we sent (reason: Server hasn't read even though they already acknoledged the communication)
             *  It's safe because we closed our previous communication and this new `Read` communication will only happen when the Server open a new `Write` communication.
            */
            
            if ((fifo = open(fifo_str, O_RDONLY)) != -1)
            {

                while ((bytes_read = read(fifo, buffer, send_bytes * sizeof (char))) > 0)
                {
                    has_error = 0;

                    for (int i = 0; !has_error && i < bytes_read; ++i)
                    {
                        status = buffer[i];
                    
                        switch (status)
                        {
                            case 0:
                                puts("Pending");
                                break;
                            
                            case 1:
                                puts("Processing");
                                break;
                            
                            default:
                                printf("Error, unknown filter: %s\n", &buffer[1]);
                                has_error = 1;

                        }
                    }
                }

                if (bytes_read == -1)
                    error = COMMUNICATION_FAILED;

                close(fifo);
            }
            else
                error = COMMUNICATION_FAILED;
            
            free(buffer);
        }

        // unlink is safe because it will only delete if no more processes are using it
        unlink(fifo_str);
        free(fifo_str);
        
    }

    free(filters_send_bytes);
    return error;
}


int
main(int argc, const char * const argv[])
{
    if (argc > 1)
    {
        Error error = SUCCESS;
        const char * const cmd = argv[1];

        if (argc == 2 && strcmp(cmd, "status") == 0)
            error = ask_status();

        else if (strcmp(cmd, "transform") == 0)
        {
            switch (argc)
            {
                case 2:
                    fputs("Missing input file, output file and filters... To know which filters type: ./" PROGRAM " status\n", stderr);
                    break;
                
                case 3:
                    fputs("Missing output file and filters... To know which filters type: ./" PROGRAM " status\n", stderr);
                    break;
                
                case 4:
                    fputs("Missing filters... To know which filters type: ./" PROGRAM " status\n", stderr);
                    break;
            
                default:
                    error = transform(argv[2], argv[3], &argv[4], (uint32_t) argc - 4);
            }          
        }

        else 
        {
            fputs("Wrong Syntax. Type one of the following commands:\n", stderr);
            print_cmds_help_to_stream(stderr);
            return -1;
        }

        if (error != SUCCESS)
            fputs(error_msg(error), stderr);
    }
    else

        print_cmds_help();

    return 0;
        
}