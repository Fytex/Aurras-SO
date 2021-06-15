#ifndef ERRORS_H
#define ERRORS_H

#define FOREACH_ERROR(_)                                                                    \
    _(                   SUCCESS, "No error\n"                                        )     \
    _(       CONFIGS_FILE_FAILED, "Configs' file couldn't be loaded\n"                )     \
    _(         NOT_ENOUGH_MEMORY, "Not enough memory\n"                               )     \
    _(         CANT_CONNECT_FIFO, "Can't to connect to repective fifo\n"              )     \
    _(           INPUT_NOT_FOUND, "No input file found\n"                             )     \
    _(            MAIN_FIFO_FAIL, "Coudn't find server's pipe to communicate\n"       )     \
    _(        SERVER_NOT_RUNNING, "Server is not listening\n"                         )     \
    _(          CANT_CREATE_FIFO, "Can't create fifo for communication\n"             )     \
    _(     CANT_CREATE_ANON_PIPE, "Can't create anonymous fifo for communication\n"   )     \
    _(      COMMUNICATION_FAILED, "Something went wrong with the communication\n"     )     \
    _(          NO_OPPOSITE_CONN, "No openning connection from the other side\n"      )     \
    _(       CANT_CREATE_PROCESS, "Can't create process...\n"                         )     \
    _(            CLIENT_TIMEOUT, "Client took too much time to send data\n"          )     \
    _(    CLIENT_CORRUPTED_DATA, "Client sent corrupted data\n"                       )     \
    _(        FILTER_NOT_EXISTS, "Filter given by the client doesn't exist"           )

#define GENERATE_ENUM(ENUM, _) ENUM,

typedef enum
{
    FOREACH_ERROR(GENERATE_ENUM)
} Error;

#define ERROR_CASE(NUM, MSG) case NUM: return MSG;


const char * error_msg(int num);

#endif // ERRORS_H