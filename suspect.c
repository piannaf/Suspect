#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>

#define BUFF_LEN 8

/* ERRORS */
#define ERR_COMMAND 1
#define ERR_BLOCK   2
#define ERR_LIMIT   3
#define ERR_OPEN    4

/* These variables are global to make things easier */
int blockCount = 1;     // Current block number
int lineCount = 1;      // Current line number
bool sawExit = false;   // Whether the block had an exit command
bool sawLimit = false;  // Whether the block had a limit command
int pRead[2];           // Pipe for reading
int pWrite[2];          // Pipe for writing
FILE *readPipe;         // For reading from the pipe
FILE *writePipe;        // For writing to the pipe
FILE *errorPipe;        // For checking whether exec succeeded
pid_t pid = -1;         // The process id, -1 means no child exists
int childStatus;        // Exit status of the child process
bool echo = false;      // For checking echo

/* Print an error message then exit the program. */
void throw_error(int code, int i, char *s)
{
    switch(code) {
        case ERR_COMMAND:
            printf("Test failed on line %d.\n", i);
            break;
        case ERR_BLOCK:
            printf("Block %d ended without an exit.\n", i);
            break;
        case ERR_LIMIT:
            printf("Block %d timed out.\n", i);
            break;
        case ERR_OPEN:
            printf("Failed to open %s.\n", s);
            break;
    }

    /* Don't try to kill a child which doesn't exist */
    if(pid != -1) {
        kill(pid, SIGINT);
    }
    exit(code);
}

/* Separates space-delimited words into an argv array 
 * Assumes cmd is a null-terminated string and holds at least one arg */
char **cmd_to_argv(char *cmd) 
{
    char *p;        // Scan the cmd string
    int argc;       // The argument count
    char **argv;    // The argument vector


    /* Count how many arguments exist */
    p = cmd;
    argc = 1;
    while(*p != '\0') {
        if(*p++ == ' ') {
            if(*p != ' ') { //don't care about extra spaces
                argc++;
            }
        }
    }

    /* Make the argument vector big enough to hold all arguments
     * plus final null pointer */
    argv = (char **)malloc(sizeof(char *) * (argc + 1));

    int l = strlen(cmd);
    char *dupCmd = (char *)malloc(sizeof(char) * (l + 1));
    strcpy(dupCmd, cmd);

    char *token = strtok(dupCmd, " ");
    for(int i = 0; i < argc; i++) {
        argv[i] = (char *)malloc(sizeof(char) * (strlen(token) + 1));
        argv[i] = token;
        token = strtok(NULL, " ");
    }
    argv[argc] = NULL;

    return argv;    
}

/* Runs a the process given by cmd as a child */
int run_new_process(char *cmd) 
{
    /* Command shouldn't start with a space */
    if(*cmd == ' ') {
        return -1;
    }
    char **argv = cmd_to_argv(cmd);
    
    /* Create the pipes */
    if(pipe(pRead) < 0 || pipe(pWrite) < 0) {
        perror("pipe failed");
        exit(errno);
    }

    /* Create a process space for program to be executed */
    if((pid = fork()) < 0) {
        perror("Fork failed");
        exit(errno);
    }

    /* Child process */
    if(!pid) {
        close(pWrite[1]);
        close(pRead[0]);

        dup2(pWrite[0], 0); // 0 is stdin
        dup2(pRead[1], 1);  // 1 is stdout

        close(pWrite[0]);
        close(pRead[1]);
    
        execvp(argv[0], argv);
        
        /* Only get here if execvp failed */
        pid_t ppid = getppid();
        kill(ppid, SIGUSR1);
    }

    /* Parent process */
    if(pid) {
        free(argv);     //Don't need to use this here so clean it up
        close(pWrite[0]);
        close(pRead[1]);
        readPipe = fdopen(pRead[0], "r");
        writePipe = fdopen(pWrite[1], "w");

        return 0;
    }
    return 0;
}

/* Get the source of input
 * If no file is specified, stdin is used */
FILE *get_input_source(int argc, char *argv[]) 
{
    if(argc == 1) {
        return stdin;
    }

    FILE *input = fopen(argv[1], "r");
    if(input == NULL) {
        throw_error(ERR_OPEN, 0, argv[1]);
    }
    return input;
}

/* Append the buffer to the line and return the new line */
char *append_to_line(char *line, char *buffer) 
{
    int newLength;
    newLength = strlen(line) + strlen(buffer);
    line = (char *)realloc(line, newLength * sizeof(char) + 1);
    if(line == NULL) {
        return NULL;
    }
    return strcat(line, buffer);
}

/* Return a complete line from the input stream */ 
char *get_line(FILE *input) 
{
    /* Initilialize an empty line */
    char *line;
    line = (char *)malloc(sizeof(char));
    line[0] = '\0';

    char buffer[BUFF_LEN];  // Hold a section of a line
    buffer[0] = '\0';

    int c;                  // Current character
    int cCount = 0;         // Number of characters read

    while((c = fgetc(input)) != EOF) {
        if(c == '\n') {
            return append_to_line(line, buffer);
        }

        if(cCount < BUFF_LEN - 1) {
            buffer[cCount] = (char)c;
            buffer[++cCount] = '\0';
        } else {
            line = append_to_line(line, buffer);

            /* Have another character to take care of */
            buffer[cCount = 0] = (char)c;
            buffer[++cCount] = '\0';
        }
    }

    /* Return NULL if we've reached EOF on a new line*/
    if(c == EOF && cCount == 0) {
        free(line); // Free the empty line
        return NULL;
    }
    /* Reached EOF but there's stuff in the buffer */
    return append_to_line(line, buffer); 
}

/* Return true if block has ended, false if not */
bool block_end(char *line) 
{
    return !strcmp(line, "");
}

/* Command Handlers */
/* Wait for the child process to exit. Pass if exit status is n.
 * Fail if program exists abnormally or exit status does not equal n.
 * Fail if n is not a positive (including 0) integer*/
int handle_exit(char *params)
{
    /* Only one exit per block. */
    if(sawExit) {
        return -1;
    }

    /* Exit takes a positive integer parameter */
    int n;                  // Store the exit parameter
    char delimiter = '\0';  // Must be space or '\0'
    
    if(params == NULL || sscanf(params, "%d%c", &n, &delimiter) < 1 ||
            n < 0 || !(delimiter == ' ' || delimiter == '\0')) {
        return -1;
    }
    sawExit = true;

    /* Wait for the child process to exit and handle appropriately */
    wait(&childStatus);
    if(WIFEXITED(childStatus)) {
        if(childStatus >> 8 == n) {
            return 1;
        }
    }

    return -1;
}

/* Read a line of text from the child process
 * Pass if it maches params. Fail if not. */
int handle_want(char *params)
{
    /* Want takes an arbitrary string parameter */
    if(params == NULL) {
        return -1;
    }

    char *line = get_line(readPipe);
    if(echo) {
        printf("%s\n", line);
    }
    if(strcmp(line, params) != 0) {
        return -1;
    }

    free(line);
    return 2;
}

/* Send params to the input of the child process
 * Pass provided there is no IO error and endinput has not been
 * executed */
int handle_send(char *params)
{
    /* Send takes an arbitrary string parameter */
    if(params == NULL) {
        return -1;
    }
    
    char *message = (char *)malloc((strlen(params) + 2) * sizeof(char));
    strcat(strcpy(message, params), "\n");

    if(fprintf(writePipe, message) < 0 || fflush(writePipe) != 0) {
        return -1;
    }

    free(message);
    return 3;
}

/* Passes if the file indicated by params exists */
int handle_exists(char *params)
{
    /* Exists takes an arbitrary string parameter */
    if(params == NULL) {
        return -1;
    }

    /* Check accessability of the file. F_OK checks existence */
    int error = access(params, F_OK);
    if(error) {
        return -1;
    }

    return 4;
}

/* Params holds an integer n and string S. Pass if the file indicated
 * by S exists and has size bigger than n. Fail otherwise. */
int handle_size(char *params)
{
    /* Size> takes two parameters:
     *  a positive integer, an arbitrary string */
    int n;          // The positive integer
    char *extra;    // The arbitrary string

    if(params == NULL) {
        return -1;
    }

    /* Size extra to fit all it would need to */
    extra = (char *)malloc(strlen(params) * sizeof(char));
    if(sscanf(params, "%d %s", &n, extra) < 2) {
        free(extra);
        return -1;
    }

    /* Check the file size */
    struct stat buffer;
    if(stat(extra, &buffer) || buffer.st_size <= n) {
        free(extra);
        return -1;
    }

    free(extra);
    return 5;
}

/* When output is read from the child process, it should be 
 * copied to stdout. The parameter determines whether echo is off or on
 * By default, echo should be off. Passes if given correct param */
int handle_echo(char *params)
{
    /* Echo takes a string paramter which must be either "on" or
     * "off" */
    char *extra; // Holds state of echo as a string
    if(params == NULL) {
        return -1;
    }

    /* Make enough space to hold whatever's thrown at us */
    extra = (char *)malloc(strlen(params) * sizeof(char));
    if(sscanf(params, "%s %*s", extra) < 1) {
        free(extra);
        return -1;
    }

    if(strcmp(extra, "on") && strcmp(extra, "off")) {
        free(extra);
        return -1;
    }   

    if(strcmp(extra, "on") == 0) {
        echo = true;    
    } else {            // Only one or the other because of above
        echo = false;
    }

    free(extra);
    return 6;
}

/* No more input will be sent to the program. Close the input pipe.
 * Always passes. */
int handle_endinput(void) 
{
    fclose(writePipe);
    return 7;
}

/* Read lines of input and send them to the program.
 * Stop doing this when W as indicated in params, appears on its own.
 * Passes provided the send operations succeed.
 * If W contains spaces, then only chars before the space will be used.
 * Interactive input is always read from stdin */
int handle_interactive(char *params)
{
    /* Interactive takes a single word parameter which must be stored
     * for future use */
    char *interactive = NULL;   // Keyword to end interactive mode
    if(params == NULL) {
        return -1;
    }

    /* Make enough space to hold whatever's thrown at us */
    interactive = (char *)malloc(strlen(params) * sizeof(char));
    if(sscanf(params, "%s %*s", interactive) < 1) {
        free(interactive);
        return -1;
    }

    /* Get lines from stdin */
    char *line;
    while((line = get_line(stdin)) != NULL) {
        if(strcmp(line, interactive) == 0) {
            break;
        }
        
        handle_send(line);
    }

    free(interactive);
    interactive = NULL;
    return 8;
}

/* Starts a timer. If a block has not been completed before the timer runs 
 * out then finish the block, kill the program and print an error message */
int handle_limit(char *params)
{
    /* there can only be one limit per block */
    if(sawLimit) {
        return -1;
    }

    /* Limit takes a non-zero positive integer paramter */
    int n;                  // Store the limit parameter
    char delimiter = '\0';  // Must be space or '\0'
    
    if(params == NULL || sscanf(params, "%d%c", &n, &delimiter) < 1 ||
            (delimiter != ' ' && delimiter != '\0')) {
        return -1;
    }
    alarm(n);

    sawLimit = true;
    return 9;
}

/* Check for valid commands, call relevant command handler */
int handle_command(char *command, char *params) 
{
    if(strcmp(command, "exit") == 0) {
        return handle_exit(params);
    }
    if(strcmp(command, "want") == 0) {
        return handle_want(params);
    }
    if(strcmp(command, "send") == 0) {
        return handle_send(params);
    }
    if(strcmp(command, "exists") == 0) {
        return handle_exists(params);
    }
    if(strcmp(command, "size>") == 0) {
        return handle_size(params);
    }
    if(strcmp(command, "echo") == 0) {
        return handle_echo(params);
    }
    if(strcmp(command, "endinput") == 0) {
        /* Endinput takes no parameters */
        return handle_endinput();
    }
    if(strcmp(command, "interactive") == 0) {
        return handle_interactive(params);
    }
    if(strcmp(command, "limit") == 0) {
        return handle_limit(params);
    }
    return -1;
}

/* Parse the file to be used as input */
void parse_input(FILE *input)
{
    int previousBlock = 0;                  // Keep track of block starts
    char *line;                             // Current line
    char *token;                            // Current command token
    char *command = NULL, *params = NULL;   // Extracted command and params

    while((line = get_line(input)) != NULL) {
        if(!block_end(line)) {
            /* First line of block is program to run */
            if(previousBlock != blockCount) {
                previousBlock = blockCount;

                /* Fork a new process */
                if(run_new_process(line)) {
                    throw_error(ERR_COMMAND, lineCount, NULL);
                }

                free(line);     // Free this line before fetching next
                ++lineCount;
                continue;
            }

            /* Either space or \n comes after command */
            token = strtok(line, " \n");
            command = token;
            while((token = strtok(NULL, "\n")) != NULL) {
                params = token;
            }
            if(handle_command(command, params) == -1) {
                throw_error(ERR_COMMAND, lineCount, NULL);
            }
            command = params = NULL;
        } else {
            /* Block has ended, init for next block */
            if(!sawExit) {
                throw_error(ERR_BLOCK, blockCount, NULL);
            }
            kill(pid, SIGINT);          // Kill the child
            pid = -1;                   // Child killed, no longer exists
            fclose(readPipe);           // No child to read from
            fclose(writePipe);          // No child to write to
            alarm(0);                   // Cancel timer
            sawLimit = sawExit = false; // Reset limit/exit
            previousBlock = blockCount; // Set blockCount
            blockCount++;
        }

        free(line); // Free this line before getting the next
        ++lineCount;
    }
}

/* Handle various signals */
void handle_sigs(int sigNum)
{
    switch(sigNum) {
        case SIGALRM:
            throw_error(ERR_LIMIT, blockCount, NULL);
        case SIGUSR1:
            --lineCount;
        case SIGPIPE:
        case SIGSEGV:
            throw_error(ERR_COMMAND, lineCount, NULL);
    }
}

int main(int argc, char *argv[])
{
    /* Set up signal handlers */
    signal(SIGALRM, handle_sigs);   // Limit timeout
    signal(SIGUSR1, handle_sigs);   // Exec failed
    signal(SIGPIPE, handle_sigs);   // Write to pipe failed
    signal(SIGSEGV, handle_sigs);   // Other stuff failed

    /* Handle user input */
    FILE *input = get_input_source(argc, argv);
    parse_input(input);

    return 0;
}

