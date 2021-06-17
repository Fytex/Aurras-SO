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

#define FILTERS_FOLDER "bin/aurrasd-filters/"
#define MAIN_FIFO "tmp/FIFO_MAIN"
#define TMP_FOLDER "tmp/"
#define PROGRAM "aurrasd"
#define CLIENT_TIMEOUT_TIME 10
#define CLIENT_MAX_SIZE UINT32_MAX

#define MSG_WAITING_TO_RUNNING "[Task started running right now]"
#define MSG_RUNNING_TO_FINISH "[Task finished right now]"

#define STRLEN(s) (sizeof(s)/sizeof(s[0])) - sizeof(s[0])

#define PAGE_SIZE 4096


typedef struct Task
{
    struct Task * next;

    char * input;
    char * output;

    uint32_t * ordered_filters;
    uint32_t * table_count_filters;

    uint32_t len_filters;

    uint32_t id;
    int fifo;
    pid_t last_filter_pid; // -1 if not running
} Task;

typedef struct
{
   Task * begin_tasks;
   Task * end_tasks;
   Task * queue_begin_tasks;
   Task * queue_end_tasks;
   uint32_t next_id;
} ManageTasks;



typedef struct 
{
    char * name;
    char * path;
    uint32_t waiting;
    uint32_t current;
    uint32_t max;
} Filter;


/*
 * These need to be global otherwise we can't access from the signal handler.
 */

static ManageTasks manage_tasks =
{
    .begin_tasks = NULL,
    .end_tasks = NULL,
    .queue_begin_tasks = NULL,
    .queue_end_tasks = NULL,
    .next_id = 0
};
static Filter * filters;
static uint32_t num_filters = 0;
static int ALARM_INTERRUPT;


static Error run_task(Task * task);


static inline void
delete_main_fifo(void)
{
    unlink(MAIN_FIFO);
}

static void
free_configs(void)
{
    for (uint32_t i = 0; i < num_filters; ++i)
    {
        free(filters[i].name);
        free(filters[i].path);
    }

    free(filters);
}

static inline void
free_task(Task * const task)
{
    free(task->input);
    free(task->output);
    free(task->ordered_filters);
    free(task->table_count_filters);
}

static Task *
find_and_remove_task_by_last_pid(pid_t pid)
{
    Task * previous = NULL;
    Task * task = manage_tasks.begin_tasks;
    
    while (task != NULL && task->last_filter_pid != pid)
    {
        previous = task;
        task = task->next;
    }

    assert(task != NULL);

    if (previous == NULL)
        manage_tasks.begin_tasks = task->next;
    else
        previous->next = task->next;

    if (manage_tasks.end_tasks == task)
        manage_tasks.end_tasks = previous;

    return task;
}

/*
 * Advantages:
 *   - Avoid accumulating zombie's processes when waiting for data in fifo
 *   - Doesn't waste CPU time
 */
static void
last_sigchld_handler(int signum)
{
    pid_t pid;
    int status;

    uint32_t * filters_available = malloc(num_filters * sizeof (uint32_t));

    if (filters_available == NULL)
    {
        fputs(error_msg(NOT_ENOUGH_MEMORY), stderr);
        exit(1);
    }


    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) // Use of a while for race signals
    {
        Task * task = find_and_remove_task_by_last_pid(pid);
        const uint32_t * const table_count_filters = task->table_count_filters;

        u8 value = 2;
        write(task->fifo, &value, sizeof (u8));
        close(task->fifo);

        for (uint32_t i = 0; i < num_filters; ++i)
        {
            filters[i].current -= table_count_filters[i];
            filters_available[i] = filters[i].max - filters[i].current;
        }

        Task * previous_new_task = NULL;
        Task * new_task = manage_tasks.queue_begin_tasks;
        
        int any_filter_available = 1;
        while (any_filter_available && new_task != NULL)
        {
            const uint32_t * const new_table_count_filters = new_task->table_count_filters;
            u8 stay_in_queue = 0;

            any_filter_available = 0;
            for (uint32_t i = 0; i < num_filters; ++i)
            {
                if (new_table_count_filters[i] > filters_available[i])
                {
                    stay_in_queue = 1;
                    filters_available[i] = 0;
                }
                else if (new_table_count_filters[i] < filters_available[i])
                {
                    filters_available[i] -= new_table_count_filters[i];
                    any_filter_available = 1;
                }
                else
                    filters_available[i] = 0;
                    
                
            }

            if (!stay_in_queue)
            {
                for (uint32_t i = 0; i < num_filters; ++i)
                {
                    filters[i].current += new_table_count_filters[i];
                    filters[i].waiting -= new_table_count_filters[i];
                }

                if (previous_new_task == NULL)
                    manage_tasks.queue_begin_tasks = new_task->next;
                else
                    previous_new_task->next = new_task->next;

                if (manage_tasks.queue_end_tasks == new_task)
                    manage_tasks.queue_end_tasks = previous_new_task;


                if (manage_tasks.end_tasks == NULL)
                    manage_tasks.begin_tasks = new_task;
                else
                    manage_tasks.end_tasks->next = new_task;
                
                manage_tasks.end_tasks = new_task;

                Error error = run_task(new_task);
                if (error == SUCCESS)
                {
                    u8 value = 1;
                    write(new_task->fifo, &value, sizeof (u8));
                }
                else
                {
                    BufferWrite buffer_write;
                    init_WriteBuffer(&buffer_write, new_task->fifo, sizeof (u8) + sizeof (uint32_t));
                    
                    u8_to_BufferWrite(&buffer_write, 3);
                    u32_to_BufferWrite(&buffer_write, error);
                    close_BufferWrite(&buffer_write);

                    fputs(error_msg(error), stderr);
                    exit(1);
                }
            }

            previous_new_task = new_task;
            new_task = new_task->next;
        }

        free_task(task);
    }
}



static void
sigint_handler(int signum)
{
    delete_main_fifo();

    // Even though begin_tasks will be filled automatically by queue_begin_tasks, there is a chance to
    // have begin_tasks empty and the queue not if we interupt (SIGINT) between the filling
    while (manage_tasks.begin_tasks != NULL || manage_tasks.queue_begin_tasks != NULL)
        pause();

    free_configs();
    exit(0);
}

static void
sigalrm_handler(int signum)
{
    ALARM_INTERRUPT = 1;
}

inline static Error
connect_main_fifo(int * fifo)
{
    if ((*fifo = open(MAIN_FIFO, O_RDONLY | O_CLOEXEC)) != -1) // O_CLOEXEC avoids children to inherit
        return SUCCESS;

    return CANT_CONNECT_FIFO;
}

inline static Error
first_connect_main_fifo(int * fifo)
{
    if (access(MAIN_FIFO, R_OK) == 0 || mkfifo(MAIN_FIFO, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) != -1) // 0666
        return connect_main_fifo(fifo);
    
    return CANT_CREATE_FIFO;
}

static Error
load_configs(const char * const configs_file, const char * const filters_folder)
{
    ssize_t bytes_read;
    char * buffer, * buffer_cursor;
    char * found;
    uint32_t size;
    Filter * filters_buffer, * filters_new_buffer;
    Filter filter;

    uint32_t total = 0;
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
                        filter.name = strdup(strsep(&found, " "));
                        
                        if (found != NULL)
                        {
                            char * relative_path = strsep(&found, " ");
                            char * path = malloc((STRLEN(FILTERS_FOLDER) + strlen(relative_path) + 1) * sizeof (char));
                            if(path != NULL)
                            {
                                filter.path = strcat(strcpy(path, FILTERS_FOLDER), relative_path);

                                if (found != NULL)
                                {
                                    filter.max = atoi(strsep(&found, " ")); // Not checking if is a valid int or if it doesn't have more arguments
                                    filter.current = 0;

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
                                
                                if (error != SUCCESS)
                                    free(path);
                            }
                            else
                                error = NOT_ENOUGH_MEMORY;
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
    Error error;
    BufferWrite buffer_write;
    Task * task;

    uint32_t total_tasks_waiting = 0;
    uint32_t total_tasks_running = 0;

    int fifo = open(fifo_str, O_WRONLY);
    if (fifo != -1)
    {
        task = manage_tasks.begin_tasks;
        while (task != NULL)
        {
            ++total_tasks_running;
            task = task->next;
        }

        task = manage_tasks.queue_begin_tasks;
        while (task != NULL)
        {
            ++total_tasks_waiting;
            task = task->next;
        }

        error = init_WriteBuffer(&buffer_write, fifo, 0);
        if (error == SUCCESS)
        {
            if ((error = u32_to_BufferWrite(&buffer_write, getpid())) == SUCCESS &&
                (error = u32_to_BufferWrite(&buffer_write, total_tasks_running)) == SUCCESS &&
                (error = u32_to_BufferWrite(&buffer_write, total_tasks_waiting)) == SUCCESS &&
                (error = u32_to_BufferWrite(&buffer_write, num_filters)) == SUCCESS)
            {

                uint32_t total = total_tasks_running;
                char * msg = MSG_RUNNING_TO_FINISH;
                ssize_t msg_size = sizeof (MSG_RUNNING_TO_FINISH);
                task = manage_tasks.begin_tasks;
            
                for (int double_cycle = 0; double_cycle < 2; ++double_cycle)
                {
                    // Can't use while (task != NULL) because meanwhile could be modified
                    for (uint32_t i = 0; error == SUCCESS && i < total; ++i)
                    {
                        if (task == NULL)
                        {
                            error = u32_to_BufferWrite(&buffer_write, -1); // -1 will become the biggest unsigned value
                            if (error == SUCCESS)
                                error = buffer_to_BufferWrite(&buffer_write, msg, msg_size); // this will probably never occur
                        }
                        else
                        {
                            error = u32_to_BufferWrite(&buffer_write, task->id);

                            uint32_t * ordered_filters = task->ordered_filters;
                            
                            if (error == SUCCESS)
                            {
                                
                                error = buffer_to_BufferWrite(&buffer_write, task->input, strlen(task->input));
                                
                                if (error == SUCCESS)
                                {
                                    error = buffer_to_BufferWrite(&buffer_write, " ", strlen(" "));
                                    if (error == SUCCESS)
                                    {
                                        error = buffer_to_BufferWrite(&buffer_write, task->output, strlen(task->output));
                                        if (error == SUCCESS)
                                            error = buffer_to_BufferWrite(&buffer_write, " ", strlen(" "));
                                    }
                                }
                            }

                            uint32_t len = task->len_filters;

                            for(uint32_t n = 0; error == SUCCESS && n < len; ++n)
                            {
                                char * filter_name = filters[ordered_filters[n]].name;
                                error = buffer_to_BufferWrite(&buffer_write, filter_name, strlen(filter_name));
                                if (error == SUCCESS)
                                    error = buffer_to_BufferWrite(&buffer_write, " ", strlen(" "));
                            }

                            if (error == SUCCESS)
                                error = buffer_to_BufferWrite(&buffer_write, "", sizeof (""));

                            task = task->next;
                        }
                    }
                    total = total_tasks_waiting;
                    msg = MSG_WAITING_TO_RUNNING;
                    msg_size = sizeof (MSG_WAITING_TO_RUNNING);
                    task = manage_tasks.queue_begin_tasks;
                }

                for (uint32_t i = 0; error == SUCCESS && i < num_filters; ++i)
                {
                    Filter filter = filters[i];

                    error = u32_to_BufferWrite(&buffer_write, filter.current);
                    if (error == SUCCESS)
                    {
                        error = u32_to_BufferWrite(&buffer_write, filter.max);
                        if (error == SUCCESS)
                            error = buffer_to_BufferWrite(&buffer_write, filter.name, strlen(filter.name) + 1);
                    }
                }
            }

            if (error != SUCCESS)
                free_WriteBuffer(&buffer_write);
        }
        else
            close(fifo);
    }
    else
        error = CANT_CONNECT_FIFO;
    
    if (error == SUCCESS)
        error = close_BufferWrite(&buffer_write);
    else if (error == CANT_CONNECT_FIFO || error == COMMUNICATION_FAILED || error == NO_OPPOSITE_CONN)
        error = SUCCESS;

    return error;
}


static Error
run_task(Task * task)
{
    Filter filter;
    uint32_t * ordered_filters = task->ordered_filters;
    uint32_t len = task->len_filters;
    int file;
    pid_t pid;

    if (len == 1)
    {
        switch ((pid = fork()))
        {
            case -1:
                return CANT_CREATE_PROCESS;
            
            case 0:
                signal(SIGINT, SIG_IGN); // otherwise keyboard interuption will propagate
                file = open(task->input, O_RDONLY);
                dup2(file, STDIN_FILENO);
                close(file);

                file = open(task->output, O_WRONLY | O_CREAT | O_TRUNC,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH); // 0666    
                dup2(file, STDOUT_FILENO);
                close(file);

                filter = filters[ordered_filters[0]];
                execlp(filter.path, filter.name, NULL);
                _exit(1);
            
            default:
                task->last_filter_pid = pid;
        }
    }
    else
    {
        int (* array_pipes)[2];
        int last_pipe[2];
        int status;

        if (pipe(last_pipe) == -1)
            return CANT_CREATE_ANON_PIPE;        

        /*
         * Use of a double fork to avoid waiting for zombies
         */
        signal(SIGCHLD, SIG_DFL); // All childs will be stacked which requires the server to wait for them

        switch ((pid = fork()))
        {
            case -1:
                _exit(CANT_CREATE_PROCESS);
            
            case 0:
                signal(SIGINT, SIG_IGN); // otherwise keyboard interuption will propagate
                close(last_pipe[0]);

                if (len > 2) // if len == 2 then we just use one pipe which will be the last one
                {
                    array_pipes = malloc((len - 2) * sizeof (int[2]));

                    if (pipe(array_pipes[0]) == -1)
                        _exit(CANT_CREATE_ANON_PIPE);
                
                }
                else
                    array_pipes = &last_pipe;


                switch (fork())
                {
                    case -1:
                        close(array_pipes[0][1]);
                        _exit(CANT_CREATE_PROCESS);
                    
                    case 0:
                        close(array_pipes[0][0]);
                        if (len > 2)
                            close(last_pipe[1]);

                        file = open(task->input, O_RDONLY);
                        dup2(file, STDIN_FILENO);
                        close(file);

                        dup2(array_pipes[0][1], STDOUT_FILENO);
                        close(array_pipes[0][1]);

                        filter = filters[ordered_filters[0]];
                        execlp(filter.path, filter.name, NULL);
                        _exit(1);
                    
                    default:
                        close(array_pipes[0][1]);
                }

                for(uint32_t i = 1; i < len - 1; ++i)
                {
                    if (i < len - 2)
                        pipe(array_pipes[i]);

                    else
                        array_pipes[i][1] = last_pipe[1];

                    switch (fork())
                    {
                        case -1:
                            return CANT_CREATE_PROCESS;

                        case 0:
                            if (i < len - 2)
                                close(last_pipe[1]);

                            close(array_pipes[i][0]);

                            dup2(array_pipes[i-1][0], STDIN_FILENO); 
                            close(array_pipes[i-1][0]);

                            dup2(array_pipes[i][1], STDOUT_FILENO);
                            close(array_pipes[i][1]);     

                            filter = filters[ordered_filters[i]];  
                            execlp(filter.path, filter.name, NULL);
                            _exit(1);

                        default:      
                            close(array_pipes[i-1][0]);
                            close(array_pipes[i][1]);
                    }
                }
            
                if (len > 2)
                    free(array_pipes);

                _exit(0);
            default:
                close(last_pipe[1]);
                waitpid(pid, &status, 0);
                signal(SIGCHLD, last_sigchld_handler); // replace the signal to the one we want
                last_sigchld_handler(0); // We don't use the value so it doesn't matter. Call this function in case there are zombies waiting for a response
                // Verificar status
            
        }

        /*
         * Last fork will become a zombie so the server can wait for them
         */
        switch ((pid = fork()))
        {
            case -1:
                return CANT_CREATE_PROCESS;

            case 0:
                signal(SIGINT, SIG_IGN); // otherwise keyboard interuption will propagate
                dup2(last_pipe[0], STDIN_FILENO);
                close(last_pipe[0]);
                
                file = open(task->output, O_WRONLY | O_CREAT | O_TRUNC,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH); // 0666
                dup2(file, STDOUT_FILENO);
                close(file);           

                filter = filters[ordered_filters[len - 1]]; 
                execlp(filter.path, filter.name, NULL);
                _exit(1);

            default:
                task->last_filter_pid = pid;
                
                close(last_pipe[0]);
                close(last_pipe[1]);                   
        }
    }

    return SUCCESS;
}

static Error
add_run_task(Task * const task)
{
    Error error = SUCCESS;
    uint32_t * table_count_filters = task->table_count_filters;

    Filter filter;
    u8 can_run = 1;

    for (uint32_t i = 0 ; i < num_filters && can_run; ++i)
    {
        filter = filters[i];

        if (filter.max - filter.current < filter.waiting + table_count_filters[i])
            can_run = 0; // Not enough resources. Either being in use or booked
    }

    Task * last_task;

    if (!can_run)
    {
        last_task = manage_tasks.queue_end_tasks;

        if (last_task == NULL)
            manage_tasks.queue_begin_tasks = task;
        else
            last_task->next = task;
        
        manage_tasks.queue_end_tasks = task;

        // Add each filter's count to the waiting list counting
        for (uint32_t i = 0; i < num_filters; ++i)
            filters[i].waiting += table_count_filters[i];
    }
    else
    {
        error = run_task(task);

        if (error == SUCCESS)
        {
            last_task = manage_tasks.end_tasks;

            if (last_task == NULL)
                manage_tasks.begin_tasks = task;
            else
                last_task->next = task;
            
            manage_tasks.end_tasks = task;

            for (uint32_t i = 0; i < num_filters; ++i)
                filters[i].current += table_count_filters[i];
        }
    }


    // if it's an error then it must be server's fault not the clients' fault
    if (error == SUCCESS)
            write(task->fifo, &can_run, sizeof (u8)); // Don't care about this result

    return error;
}


static Error
create_task_from_filters_array(char * const input, char * const output,
    uint32_t * const table_count_filters, uint32_t * const ordered_filters, uint32_t len, const int fifo)
{
    Task * task = malloc(sizeof (Task));

    if (!task)
        return NOT_ENOUGH_MEMORY;

    *task = (Task)
    {
        .fifo = fifo,
        .id = manage_tasks.next_id++, // It's not UB. Source: GNU C Manual A.1 Basics of Integer Overflow
        .last_filter_pid = -1,
        .len_filters = len,
        .next = NULL,
        .ordered_filters = ordered_filters,
        .table_count_filters = table_count_filters,
        .input = input,
        .output = output
    };

    return add_run_task(task);

}



static Error
transform(const char * const fifo_str, ssize_t total_bytes)
{
    Error error;
    BufferRead buffer_read;
    int fifo = open(fifo_str, O_RDONLY | O_CLOEXEC); // O_CLOEXEC avoids children to inherit

    if (fifo != -1)
    {
        if (total_bytes <= CLIENT_MAX_SIZE)
        {
        
            error = init_ReadBuffer(&buffer_read, fifo, total_bytes);
            if (error == SUCCESS)
            {
                ALARM_INTERRUPT = 0;
                alarm(CLIENT_TIMEOUT_TIME); // Avoid waiting
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
                        ssize_t size = 8;
                        uint32_t * ordered_filters = malloc(size * sizeof (uint32_t));
                        uint32_t * new_buffer;
                        ssize_t len = 0;

                        if (ordered_filters != NULL)
                        {
                            uint32_t * table_count_filters = calloc(num_filters, sizeof (uint32_t));

                            if (table_count_filters)
                            {

                                char * input = NULL, * output = NULL;

                                do
                                {
                                    if (input == NULL)
                                    {
                                        if (access(buffer_read.cursor, R_OK) != 0)
                                        {
                                            error = INPUT_NOT_FOUND;
                                            break;
                                        }
                                        input = strdup(buffer_read.cursor);
                                        if (input == NULL)
                                        {
                                            error = NOT_ENOUGH_MEMORY;
                                            break;
                                        }
                                    }
                                    else if (output == NULL)
                                    {
                                        output = strdup(buffer_read.cursor);
                                        if (output == NULL)
                                        {
                                            error = NOT_ENOUGH_MEMORY;
                                            break;
                                        }
                                    }
                                    else
                                    {
                                        uint32_t i;
                                        for (i = 0; i < num_filters; ++i)
                                        {
                                            if (strcmp(buffer_read.cursor, filters[i].name) == 0)
                                            {
                                                ++table_count_filters[i];

                                                if (table_count_filters[i] > filters[i].max)
                                                {
                                                    error = FILTER_EXCEEDS_MAX;
                                                    break;
                                                }

                                                if (size == len)
                                                {
                                                    size *= 2;
                                                    new_buffer = realloc(ordered_filters, size * sizeof (uint32_t));
                                                    if (new_buffer != NULL)
                                                    {
                                                        ordered_filters = new_buffer;
                                                        ordered_filters[len++] = i;
                                                    }
                                                    else
                                                        error = NOT_ENOUGH_MEMORY;
                                                }
                                                else
                                                    ordered_filters[len++] = i;

                                                break;
                                            }
                                        }

                                        if (error != SUCCESS)
                                            break;

                                        if (i == num_filters)
                                        {
                                            error = FILTER_NOT_EXISTS;
                                            break;
                                        }
                                    }

                                    size = strlen(buffer_read.cursor) + 1;
                                    buffer_read.cursor += size;
                                    total_bytes -= size;
                                } while (total_bytes > 0);

                                close(fifo);

                                fifo = open(fifo_str, O_WRONLY | O_CLOEXEC); // O_CLOEXEC avoids children to inherit
                                if (fifo == -1)
                                    error = CANT_CONNECT_FIFO;

                                if (error == SUCCESS)
                                    error = create_task_from_filters_array(input, output, table_count_filters, ordered_filters, len, fifo);

                                if (error != SUCCESS)
                                {
                                    if (input != NULL)
                                    {
                                        free(input);

                                        if (output != NULL)
                                            free(output);
                                    }
                                    free(table_count_filters);
                                }
        
                            }
                            else
                                error = NOT_ENOUGH_MEMORY;

                            if (error != SUCCESS)
                                free(ordered_filters);
                        }
                        else
                            error = NOT_ENOUGH_MEMORY;
                    }
                    
                }

                free_ReadBuffer(&buffer_read);
            }
            else
                error = NOT_ENOUGH_MEMORY;
        }
        else
            error = CLIENT_EXCEEDS_SIZE;
    }
    else
        error = CANT_CONNECT_FIFO;

    if (error != SUCCESS && error != CANT_CONNECT_FIFO && error != NO_OPPOSITE_CONN) {
        BufferWrite buffer_write;
        init_WriteBuffer(&buffer_write, fifo, 0);

        u8_to_BufferWrite(&buffer_write, 3);
        u32_to_BufferWrite(&buffer_write, error);

        close_BufferWrite(&buffer_write);

        if (error == INPUT_NOT_FOUND || error == CLIENT_EXCEEDS_SIZE || error == FILTER_EXCEEDS_MAX || error == FILTER_NOT_EXISTS || error == COMMUNICATION_FAILED)
            error = SUCCESS; // handle error

    }
    else
        error = SUCCESS; // Either there was no error or couldn't connect to fifo which is not a server's problem

    return error;   
}

static Error
parse_execute_task(pid_t client_pid, ssize_t total_bytes)
{
    Error error = SUCCESS;
    ssize_t fifo_str_len = STRLEN(TMP_FOLDER) + snprintf(NULL, 0, "%d", client_pid);
    char * fifo_str = malloc((fifo_str_len + 1) * sizeof (char));
    if(fifo_str == NULL)
        return NOT_ENOUGH_MEMORY;

    snprintf(fifo_str, fifo_str_len + 1, "%s%d", TMP_FOLDER, client_pid); // Using same function to garantee same code

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
        error = first_connect_main_fifo(&main_fifo);

        if (error == SUCCESS)
        {
            error = init_ReadBuffer(&buffer_read, main_fifo, 0);
            uint32_t client_pid, total_bytes;

            while (error == SUCCESS)
            {
                if ((error = u32_from_BufferRead(&buffer_read, &client_pid)) == SUCCESS)
                    error = u32_from_BufferRead(&buffer_read, &total_bytes);

                if (error == NO_OPPOSITE_CONN) {
                    close(main_fifo);
                    error = connect_main_fifo(&main_fifo); // This way we can avoid active waiting
                    if (error == SUCCESS)
                        reset_ReadBuffer_file(&buffer_read, main_fifo);

                    continue; // if no clients then it will restart the loop. If this error happens on the next functions than it is a serious problem... Better shutdown the server than mess it up
                }
                else if (error != SUCCESS)
                    break;

                error = parse_execute_task(client_pid, total_bytes);
            }

            free_ReadBuffer(&buffer_read);

            delete_main_fifo();
        }
        free_configs();
    }

    return error;
}


int
main(int argc, char * argv[])
{
    if (argc == 3)
    {
        Error error;
        
        signal(SIGINT, sigint_handler);
        signal(SIGCHLD, last_sigchld_handler);
        signal(SIGALRM, sigalrm_handler);
        signal(SIGPIPE, SIG_IGN); // Ignore if client kills while server is communicating
        siginterrupt(SIGALRM, 1); // Alarm's signal will no longer restart `read` function

        error = run(argv[1], argv[2]);

        if (error != SUCCESS)
        {
            fputs(error_msg(error), stderr);
            return 1;
        }

    }
    else
        fputs("./" PROGRAM " config-filename filters-folder\n", stderr);

    return 0;
}