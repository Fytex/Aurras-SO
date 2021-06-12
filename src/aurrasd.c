#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <inttypes.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>

#include "buffer_manager.h"
#include "errors.h"

#define MAIN_FIFO "tmp/FIFO_MAIN"
#define TMP_FOLDER "tmp/"
#define PROGRAM "aurrasd"

#define STRLEN(s) (sizeof(s)/sizeof(s[0])) - sizeof(s[0])

#define PAGE_SIZE 4096


typedef struct
{
    Task * next;
    int32_t * n_procs_by_filters;
    int32_t num_task;
    int32_t count_not_completed;
    int fifo;
} Task;


typedef struct
{
    Process * next;
    int32_t num_task;
    pid_t pid;
} Process;

typedef struct 
{
    Process * queue_begin;
    Process * queue_end;
    char * filter_name;
    char * filter_path;
    uint32_t queue_last_idx;
    uint32_t queue_cap;
    uint32_t current;
    uint32_t max;
} ProcessesByFilter;

/*
 * These need to be global otherwise we can't access from the signal handler.
 */

static Task * tasks;
static ProcessesByFilter * processes_by_filter;
static uint32_t num_filters = 0;
static int ALARM_INTERRUPT;

void
delete_main_fifo(void)
{

}

void
free_configs(void)
{
}

/*
 * Advantages:
 *   - Avoid accumulating zombie's processes when waiting for data in fifo
 *   - Doesn't waste CPU time
 */

void 
sigchld_handler(int signum)
{
    pid_t pid;
    int status;

    Child * children = child_processes.children;
    int32_t last_idx = child_processes.last_idx;
    int32_t filter_num = -1;
    int32_t current;


    while ((pid = waitpid(-1, &status, WNOHANG)) != -1) // Use of a while for race signals
    {
        for (int32_t i = 0; i <= last_idx; ++i)
        {
            if (children[i].pid == pid)
            {
                children[i].pid = -1;
                filter_num = children[i].filter;
                //free(children[i].cmd);

                if (i == last_idx)
                    --child_processes.last_idx;

                break; 
            }
        }
        
        assert(filter_num >= 0);

        current = --(filters[filter_num].current);

        assert(current >= 0);
    }
}


void
sigint_handler(int signum)
{
    delete_main_fifo();
    free_configs();
}

void
sigalrm_handler(int signum)
{
    ALARM_INTERRUPT = 1;
}

inline static Error
connect_main_fifo(int * fifo)
{
    if (access(MAIN_FIFO, R_OK) == 0 || mkfifo(MAIN_FIFO, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) != -1) // 0666
    {
        if ((*fifo = open(MAIN_FIFO, O_RDONLY)) != -1)
            return SUCCESS;
        else
            return CANT_CONNECT_FIFO;
    }
    
    return CANT_CREATE_FIFO;
}

static Error
load_configs(const char * const configs_file, const char * const filters_folder)
{
    ssize_t bytes_read;
    char * buffer, * buffer_cursor;
    char * found;
    int32_t size;
    Filter * filters_buffer, * filters_new_buffer;
    Filter filter;

    int32_t total = 0;
    Error error = SUCCESS;
    int file = open(configs_file, O_RDONLY);

    if (file != -1)
    {
        buffer_cursor = buffer = malloc((PAGE_SIZE + 1) * sizeof (char));
        buffer[PAGE_SIZE] = '\0'; // Avoid buffer overrun when calling strsep

        if (buffer != NULL)
        {
            size = 8;
            filters_buffer = malloc(size * sizeof (Filter));

            if (filters_buffer != NULL)
            {
                while (error == SUCCESS && (bytes_read = read(file, buffer, PAGE_SIZE * sizeof (char))) > 0)
                {
                    // if bytes_read < supposed number of bytes to read then it means we reached end of file (last string is entirely in buffer)
                    while (error == SUCCESS && (((found = strsep(&buffer_cursor, "\n")) && *found != '\0') && (buffer_cursor != NULL || bytes_read < PAGE_SIZE * sizeof (char))))
                    {
                        filter.filter_name = strdup(strsep(&found, " "));
                        
                        if (found != NULL)
                        {
                            filter.filter_path = strdup(strsep(&found, " "));

                            if (found != NULL)
                            {
                                filter.max = atoi(strsep(&found, " ")); // Not checking if is a valid int or if it doesn't have more arguments
                                filter.current = 0;

                                // FAZER
                                // FUNÇÃO
                                // RETORNAR
                                // ERROR E FILTER
                                // CORTAR CODE
                                if (size == total)
                                {
                                    size *= 2;
                                    filters_new_buffer = realloc(filters_buffer, size * sizeof (Filter));

                                    if (filters_new_buffer != NULL)
                                    {
                                        filters_buffer = filters_new_buffer;
                                        filters_buffer[total++] = filter;
                                        
                                    }
                                    else
                                        error = NOT_ENOUGH_MEMORY;
                                        
                                }
                                else
                                    filters_buffer[total++] = filter;

                            }
                            else
                                error = CONFIGS_FILE_FAILED;
                        }
                        else
                            error = CONFIGS_FILE_FAILED;
                        
                    }
                    
                }

                if (bytes_read != -1)
                {
                    // set buffer and count to the respective global variables
                    filters = filters_buffer;
                    num_filters = total;
                }
                else
                    error = CONFIGS_FILE_FAILED;
            }
            else
                error = NOT_ENOUGH_MEMORY;

            free(buffer);
        }
        else
            error = NOT_ENOUGH_MEMORY;
        
        close(file);
    }
    else
        error = CONFIGS_FILE_FAILED;

    return error;
}

static Error
status(const char * const fifo_str)
{
    int fifo = open(fifo_str, O_WRONLY);
    if (fifo == -1)
        return CANT_CONNECT_FIFO;
    
    BufferWrite buffer_write;
    int32_t buffer[10000];
    buffer_write.buffer = buffer;
    buffer_write.cap = 10000;
    buffer_write.len = 0;
    u32_to_BufferWrite(&buffer_write, 1633837924);
    u32_to_BufferWrite(&buffer_write, 1);
    u32_to_BufferWrite(&buffer_write, 1);
    u32_to_BufferWrite(&buffer_write, 58);
    buffer_to_BufferWrite(&buffer_write, "./aurras askdfl sdfkl sdf", sizeof ("./aurras askdfl sdfkl sdf"));
    u32_to_BufferWrite(&buffer_write, 1633837924);
    u32_to_BufferWrite(&buffer_write, 1633837924);
    buffer_to_BufferWrite(&buffer_write, "one-two", sizeof ("one-two"));
    write(1, buffer_write.buffer, buffer_write.len);
    write(fifo, buffer_write.buffer, buffer_write.len);
}





static Error
transform(const char * const fifo_str, ssize_t total_bytes)
{
    Error error;
    BufferRead buffer_read;
    uint32_t * exec_filters;
    int fifo = open(fifo_str, O_RDONLY);

    if (fifo == -1)
        return CANT_CONNECT_FIFO;
    
    exec_filters = malloc(num_filters * sizeof (uint32_t));
    if (exec_filters != NULL)
    {
        error= init_ReadBuffer(&buffer_read, fifo, total_bytes);
        if (error == SUCCESS)
        {
            ALARM_INTERRUPT = 0;
            alarm(10); // Avoid waiting 
            error = read_at_least(&buffer_read, total_bytes);
            alarm(0); // reset alarm

            if (ALARM_INTERRUPT == 1)
                error = CLIENT_TIMEOUT;
            else if (error == SUCCESS) // last char must be '\0' otherwise we can have trouble with buffer overrun
            {
                
                if (((char *) buffer_read.buffer)[buffer_read.len - 1] != '\0')
                    error = CLIENT_CORRUPTED_DATA;
                else
                {
                    uint32_t * dict_filters = calloc(num_filters * sizeof (uint32_t));
                    ssize_t len;

                    if (dict_filters != NULL)
                    {
                        ProcessesByFilter * proc_by_filters = processes_by_filter; // bring it locally
                        ssize_t size;
                        do
                        {
                            uint32_t i;
                            for (i = 0; i < num_filters; ++i)
                            {
                                if (strcmp(buffer_read.cursor, processes_by_filter[i].filter_name) == 0)
                                {
                                    ++dict_filters[i];
                                    break;
                                }
                            }

                            if (i == num_filters)
                            {
                                error = FILTER_NOT_EXISTS;
                                break;
                            }

                            size = strlen(buffer_read.cursor) + 1;
                            buffer_read.cursor += size;
                            total_bytes -= size;

                        } while (total_bytes > 0);

                        if (error == SUCCESS)
                            run_filters_from_array(dict_filters);

                        free(dict_filters);
                    }
                    else
                        error = NOT_ENOUGH_MEMORY;
                }
                
            }

            free(buffer_read.buffer);
        }
        else
            error = NOT_ENOUGH_MEMORY;

        free(exec_filters);
    }
    else
        error = NOT_ENOUGH_MEMORY;

    close(fifo); 

    return error;   
}

static Error
execute_task(int client_pid, ssize_t total_bytes)
{
    Error error;
    ssize_t fifo_str_len = STRLEN(TMP_FOLDER) + snprintf(NULL, 0, "%d", client_pid);
    char * fifo_str = malloc((fifo_str_len + 1) * sizeof (char));
    if(fifo_str == NULL)
        return NOT_ENOUGH_MEMORY;

    snprintf(fifo_str, fifo_str_len, "%s%d", TMP_FOLDER, client_pid); // Using same function to garantee same code

    error = (total_bytes == 0) ? status(fifo_str) : transform(fifo_str, total_bytes);

    free(fifo_str);

    return error;
}


static Error
run(const char * const configs_file, const char * const filters_folder)
{
    BufferRead buffer_read;
    Error error;
    int main_fifo;
    
    error = load_configs(configs_file, filters_folder);

    if (error == SUCCESS)
    {
        error = connect_main_fifo(&main_fifo);

        if (error == SUCCESS)
        {
            Error error = init_ReadBuffer(&buffer_read, main_fifo, 0);
            uint32_t client_pid, total_bytes;

            while (error == SUCCESS)
            {
                error = u32_from_BufferRead(&buffer_read, &client_pid);
                printf("PID: %d\n", client_pid);
                if (error == NO_OPPOSITE_CONN)
                    continue; // if no clients then it will restart the loop. If this error happens on the next functions than it is a serious problem... Better shutdown the server than mess it up
                else if (error != SUCCESS)
                    break;
                
                error = u32_from_BufferRead(&buffer_read, &total_bytes);
                if (error != SUCCESS)
                    break;

                error = execute_task(client_pid, total_bytes);
            }

            delete_main_fifo();
        }
        free_configs();
    }
}


int
main(int argc, char * argv[])
{
    if (argc == 3)
    {
        Error error;
        
        signal(SIGINT, sigint_handler);
        signal(SIGCHLD, sigchld_handler);
        signal(SIGALRM, sigalrm_handler);
        siginterrupt(SIGALRM, 1); // Alarm's signal will no longer restart `read` function

        error = run(argv[1], argv[2]);

        if (error != SUCCESS)
            fputs(error_msg(error), stderr);

    }
    else
        fputs("./" PROGRAM " config-filename filters-folder\n", stderr);

    return 0;
}