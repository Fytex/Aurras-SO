#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>

#define MAIN_FIFO "tmp/FIFO_MAIN"
#define PROGRAM "aurras"
#define PAGE_SIZE 4096

#define FOREACH_ERROR(_)                                                                    \
    _(                  SUCCESS,  "No error\n"                                        )     \
    _(           INPUT_NOT_FOUND, "No input file found\n"                             )     \
    _(            MAIN_FIFO_FAIL, "Coudn't find server's pipe to communicate\n"       )     \
    _(        SERVER_NOT_RUNNING, "Server is not listening\n"                         )     \
    _(          CANT_CREATE_FIFO, "Can't create pipe of communication\n"              )     \
    _(      COMMUNICATION_FAILED, "Something went wrong with the communication\n"     )     \
    _(         NOT_ENOUGH_MEMORY, "Not enough memory\n"                               )

#define GENERATE_ENUM(ENUM, _) ENUM,
#define ERROR_CASE(NUM, MSG) case NUM: return MSG;

typedef enum {
    FOREACH_ERROR(GENERATE_ENUM)
} Error;

static const char * error_msg(const int num)
{
    // Since this returns a const char * allocated at compile time typically placed in .rodata
    // So there won't be any Undefined Behaviour

    switch (num) {
        FOREACH_ERROR(ERROR_CASE)
    }

    return "Unknown error";
}

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
create_connection(char ** const fifo_str, int32_t send_bytes)
{
    int main_fifo;
    pid_t pid;
    char * pid_str;
    Error error = SUCCESS;
    int32_t data[2];

    if ((main_fifo = open(MAIN_FIFO, O_WRONLY)) != -1)
    {
        pid = getpid();
        pid_str = malloc((snprintf(NULL, 0, "%d", pid) + 1) * sizeof (char));

        if (pid_str != NULL)
        {

            snprintf(pid_str, 0, "%d", pid); // Using same function to garantee same code

            if (access(pid_str, R_OK | W_OK) == 0 || mkfifo(pid_str, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) != -1) // 0666
            {
                // The pid_t data type is a signed integer type which is capable of representing a process ID. In the GNU C Library, this is an int.
                // int in *nix at the moment are always 2 or 4 bytes
                data[0] = (int32_t) pid;
                data[1] = send_bytes;

                if (write(main_fifo, data, sizeof (data)) != -1)
                    *fifo_str = pid_str;
                else 
                {
                    free(pid_str);
                    error = COMMUNICATION_FAILED;
                }

            }
            else 
            {
                free(pid_str);
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


/*
 *  This works as a clock buffer. If the final size is bigger than buffer + PAGE_SIZE then it will copy all unused values to the beginning
 *  We are considering that at_least is always <= PAGE_SIZE
 */
static ssize_t
read_at_least(int fifo, void * const buffer, void ** const ext_buffer_cursor, ssize_t still_have, ssize_t at_least)
{
    ssize_t bytes_read;
    ssize_t total_available = still_have;
    void * buffer_cursor = *ext_buffer_cursor;

    // Better to copy memory then do more calls to read from disk
    if (still_have < at_least)
    {
        if (still_have == 0)
            *ext_buffer_cursor = buffer_cursor = buffer;

        if ((buffer_cursor - buffer) >= still_have) // Memory can't overlap with memcpy
        {
            memcpy(buffer, buffer_cursor, still_have);
            *ext_buffer_cursor = buffer_cursor = buffer;
        }
        else if ((buffer_cursor - buffer) + at_least > PAGE_SIZE) // if with the buffer we have it still exceds the page size
        {
            memmove(buffer, buffer_cursor, still_have);
            *ext_buffer_cursor = buffer_cursor = buffer;
        }
    }

    buffer_cursor += still_have;

    if (at_least == 0) // If not specified then we give the first block we receive
        at_least = 1;


    while (total_available < at_least)
    {
        bytes_read = read(fifo, buffer, PAGE_SIZE - total_available);
        buffer_cursor += bytes_read;
        total_available += bytes_read;
    }

    return total_available;
}

static Error
ask_status(void)
{
    Error error;
    char * fifo_str;
    int32_t * buffer;
    int32_t * buffer_cursor;
    int fifo;
    pid_t server_pid;
    int32_t n_tasks, n_filters;
    ssize_t remaining;
    int32_t running, max;

    error = create_connection(&fifo_str, 0); // if we don't send any bytes then must be status command

    if (error != SUCCESS)
        return error;
    
    buffer = buffer_cursor = malloc(PAGE_SIZE);

    if (buffer != NULL)
    {

        if ((fifo = open(fifo_str, O_WRONLY)) != -1)
        {
            remaining = read_at_least(fifo, buffer, (void **) &buffer_cursor, 0, 3 * sizeof (int32_t));
  
            // There must be content in the first time we read otherwise it's an error
            if (remaining != -1)
            {        
                server_pid = (pid_t) *buffer_cursor++; // First 4 bytes for Server's pid
                n_tasks = *buffer_cursor++; // Second 4 bytes for Number of tasks
                n_filters = *buffer_cursor++; // Third 4 bytes for Number of filters
                remaining = remaining - (3 * sizeof (int32_t));

                for (int32_t i = 0; i < n_tasks; ++i)
                {
                    remaining = read_at_least(fifo, buffer, (void **) &buffer_cursor, remaining, sizeof (int32_t) + sizeof ""); // Num + string minimum size

                    printf("Task #%" PRId32 ": transform ", *buffer_cursor++);

                    remaining -= sizeof (int32_t);

                    /*
                        If last character is '\0' then remainig will stay with value 1
                    */
                    while ((remaining -= fprintf(stdout, "%.*s", (int) remaining, (char *) buffer_cursor)) == 0) // still same line
                        remaining = read_at_least(fifo, buffer, (void **) &buffer_cursor, 0, 0);

                    buffer_cursor = &buffer[PAGE_SIZE - remaining];
                    puts("");
                    
                }

                for (int32_t i = 0; i < n_filters; ++i)
                {
                    read_at_least(fifo, buffer, (void **) &buffer_cursor, remaining, 2 * sizeof (int32_t) + sizeof ""); // 2 Nums + string minimum size
                    running = ((int32_t *) buffer_cursor)[0];
                    max = ((int32_t *) buffer_cursor)[1];

                    remaining -= 2 * sizeof (int32_t);

                    printf("Filter ");

                    /*
                        If last character is '\0' then remainig will stay with value 1
                    */
                    while ((remaining -= fprintf(stdout, "%.*s", (int) remaining, (char *) &buffer_cursor[1])) == 0) // still same line
                        remaining = read_at_least(fifo, buffer, (void **) &buffer_cursor, 0, 0);
                    
                    printf(": %d/%d (running/max)\n", running, max);
                }

                printf("pid: %d", server_pid);
            }
            else
                error = COMMUNICATION_FAILED;

        }

    }
    
    free(fifo_str);
    return SUCCESS;

}


static Error
transform(const char * const input, const char * const output, const char * const * const filters, int32_t n_filters)
{
    Error error;
    int32_t send_bytes;
    int32_t * filters_send_bytes;
    unsigned char * buffer, * buffer_cursor;
    char * fifo_str;
    int fifo;
    int8_t status;
    ssize_t bytes_read;
    int has_error;

    // No need to send an invalid message to the server since it has some costs...
    if (access(input, F_OK) == -1)
        return INPUT_NOT_FOUND;

    filters_send_bytes = malloc(n_filters * sizeof (size_t));

    if (filters_send_bytes == NULL)
        return NOT_ENOUGH_MEMORY;

    send_bytes = 0;

    for (int i = 0; i < n_filters; ++i)
        send_bytes += (filters_send_bytes[i] = strlen(filters[i]) + 1);

    error = create_connection(&fifo_str, send_bytes);

    if (error == SUCCESS)
    {
    
        if ((fifo = open(fifo_str, O_WRONLY)) != -1) {

            buffer = malloc(send_bytes * sizeof (char));

            if (buffer != NULL)
            {
                buffer_cursor = buffer;

                for (int i = 0; i < n_filters; ++i)
                {
                    memcpy(buffer_cursor, filters[i], filters_send_bytes[i]);
                    buffer_cursor += filters_send_bytes[i];
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

                close(fifo);
            }
            else
                error = COMMUNICATION_FAILED;
            
            free(buffer);
        }

        unlink(fifo_str);
        free(fifo_str);
        
    }

    free(filters_send_bytes);
    return error;
}


int
main(int32_t argc, const char * const argv[])
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
                    error = transform(argv[2], argv[3], &argv[4], argc - 4);
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