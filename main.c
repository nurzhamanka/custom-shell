/*
 *
 * nurzhan shell aka nsh
 * operating systems spring 2018
 *
 * implemented:
 * - commands with arguments
 * - pipelining
 * - i/o redirection
 * - pipes + i/o redir combined
 * - running programs in the background
 * - backing up to a file if provided as a cl arg
 *
 * references:
 * - inspiration for the structure of my program: https://brennan.io/2015/01/16/write-a-shell-in-c/
 * - inspiration for using structs to store commands: https://stackoverflow.com/questions/8082932/connecting-n-commands-with-pipes-in-a-shell
 *
 * */

#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>

#define TKS_BUFFER_SIZE 128
#define READ 0
#define WRITE 1

typedef struct
{
    int num_args;
    char** args;
} Command;

typedef struct
{
    int num_cmds;
    Command* cmds;
    char* file_out;
    char* file_in;
    int overwrite;
} FullCommand;

int bg;
int fd;
int backup;
char* fname;

void loop(void);
char* get_cmd(void);
FullCommand* cmd_builder(char* line);
int execute_cmd(FullCommand* cmd, int bg_flag);
void print_command(FullCommand* cmd);

int main(int argc, char* argv[])
{
    /* i decided to add some abstraction
     * and break down the program into
     * functions and all kinds of cute stuff */

    // output file management
    fd = -1;
    if (argc == 1) {
        // no output file
    } else if (argc == 2) {
        // yes output file
        fd = open(argv[1], O_WRONLY | O_APPEND | O_CREAT, 0666);
        if (fd == -1) {
            perror("nsh");
            return EXIT_FAILURE;
        }
        backup = 1;
        fname = argv[1];
    } else {
        fprintf(stderr, "nsh usage: \'./nsh filename\'\n");
        return EXIT_FAILURE;
    }

    // the command loop
    loop();

    if (fd != -1)
        close(fd);
    return EXIT_SUCCESS;
}

void loop(void)
{
    /* the command loop of the shell */

    FullCommand* cmd;
    char* in;
    int status;

    do {
        fprintf(stdout, "> ");
        in = get_cmd();

        if (backup) {
            write(fd, "> ", 3);
            write(fd, in, strlen(in) + 1);
            write(fd, "\n", 2);
        }

        cmd = cmd_builder(in);
        status = execute_cmd(cmd, bg);

        // freeing up the command
        for (int x = 0; x < 16; x++)
            free(cmd->cmds[x].args);
        free(cmd->cmds);
        free(cmd);
    } while (status);
}

char* get_cmd(void)
{
    /* fgets() from simple_shell.c is deprecated,
     * so i used getline() to read input */

    char* line = NULL;
    size_t bsize = 0;
    getline(&line, &bsize, stdin);

    line[strlen(line)-1] = '\0';

    if (line[strlen(line)-1] == '&') {
        bg = 1;
        line[strlen(line)-1] = '\0';
    }

    return line;
}

/*
 * cmd_builder() takes in the whole input line
 * and breaks it down into tokens, which are
 * put then into structs for commands, which then
 * are put into a full command
 *
 * it uses strtok with some array index manipulation
 *
 * the advantage of this is that it is very simple now
 * to combine pipes and i/o redirection, and, in general,
 * it makes my code a lot cleaner
 *
 * if it detects that the user wants to backup,
 * it will just add a pipe to tee at the end
 * */

FullCommand* cmd_builder(char* line)
{
    FullCommand* fcmdp = (FullCommand*)malloc(sizeof(FullCommand));
    fcmdp->cmds = (Command*)malloc(16 * sizeof(Command));
    for (int x = 0; x < 16; x++)
        fcmdp->cmds[x].args = (char**)malloc(101 * sizeof(char*));

    int bsize = TKS_BUFFER_SIZE;
    int pos = 0;
    char** toks = (char**)malloc(bsize * sizeof(char*));
    char* tok;

    int num_cmd = 0;
    int offset = 0;
    char* file_in = NULL;
    char* file_out = NULL;

    if (!toks) {
        fprintf(stderr, "nsh: malloc error\n");
        exit(EXIT_FAILURE);
    }

    tok = strtok(line, " ");
    while (tok != NULL) {

        toks[pos] = tok;
        fcmdp->cmds[num_cmd].args[offset] = tok;

        if (strcmp(tok, "|") == 0) {
            /* pipeline detected:
             * shift to the next command in the line*/
            fcmdp->cmds[num_cmd].args[offset] = NULL;
            offset = 0;
            num_cmd++;
            pos++;
        } else if (pos > 0 && strcmp(toks[pos-1], ">") == 0) {
            /* output file (overwrite) detected */
            file_out = tok;
            fcmdp->cmds[num_cmd].args[offset-1] = NULL;
            fcmdp->overwrite = 1;
            pos++;
        } else if (pos > 0 && strcmp(toks[pos-1], "<") == 0) {
            /* input file detected */
            file_in = tok;
            fcmdp->cmds[num_cmd].args[offset-1] = NULL;
            pos++;
        } else if (pos > 0 && strcmp(toks[pos-1], ">>") == 0) {
            /* output file (append) detected */
            file_out = tok;
            fcmdp->cmds[num_cmd].args[offset-1] = NULL;
            fcmdp->overwrite = 0;
            pos++;
        } else {
            pos++;
            offset++;
            fcmdp->cmds[num_cmd].num_args++;
        }

        tok = strtok(NULL, " ");
    }

    fcmdp->cmds[num_cmd].args[offset] = NULL;

    if (backup) {
        offset = 0;
        num_cmd++;
        fcmdp->cmds[num_cmd].args[offset++] = "tee";
        fcmdp->cmds[num_cmd].args[offset++] = "-a";
        fcmdp->cmds[num_cmd].args[offset++] = fname;
        fcmdp->cmds[num_cmd].args[offset] = NULL;
    }

    fcmdp->num_cmds = num_cmd + 1;
    fcmdp->file_in = file_in;
    fcmdp->file_out = file_out;

    /* we actually don't need toks, because
     * we only used it to create FullCommand */
    free(toks);

    return fcmdp;
}

int execute_cmd(FullCommand* cmd, int bg_flag)
{
    if (strcmp(cmd->cmds->args[0], "quit") == 0) {
        return 0;
    }

    /* save stdin and stdout
     * to restore them later */
    int tin = dup(0);
    int tout = dup(1);

    /* check first if there is an input file
     * if there is not, use stdin */

    int fd_in, fd_out;
    if (cmd->file_in != NULL) {
        fd_in = open(cmd->file_in, O_RDONLY, 0444);
        if (fd_in < 0) {
            perror("nsh");
            exit(EXIT_FAILURE);
        }
    } else {
        fd_in = dup(tin);
    }

    int pid;
    int status;
    int num_cmds = cmd->num_cmds;

    /* process each command in the pipeline */
    for (int i = 0; i < num_cmds; i++) {

        // redirect input
        dup2(fd_in, 0);
        close(fd_in);

        if (i == cmd->num_cmds - 1) {

            /* last command, which means
             * we should check if there is
             * an output file we can redirect to */

            if (cmd->file_out != NULL) {
                if (cmd->overwrite) {
                    fd_out = open(cmd->file_out, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                } else {
                    fd_out = open(cmd->file_out, O_WRONLY | O_APPEND | O_CREAT, 0666);
                }
                if (fd_in < 0) {
                    perror("nsh");
                    exit(EXIT_FAILURE);
                }
            } else {
                // or use stdout
                fd_out = dup(tout);
            }
        }
        else {
            // create a pipe for... well... pipelining, i guess
            int fds[2];
            pipe(fds);
            fd_out = fds[WRITE];
            fd_in = fds[READ];
        }

        // redirect output
        dup2(fd_out, 1);
        close(fd_out);

        pid = fork();
        if (pid == 0) {
            /* the child process */
            if (execvp(cmd->cmds[i].args[0], cmd->cmds[i].args) == -1) {
                perror("nsh");
                exit(EXIT_FAILURE);
            }
        }
    }

    /* restore stdin and stdout */
    dup2(tin, 0);
    dup2(tout, 1);
    close(tin);
    close(tout);

    if (!bg_flag) {
        waitpid(pid, &status, 0);
    }

    return 1;
}

/* function to print out the Full Command
 * neatly for debugging */

void print_command(FullCommand* cmd)
{
    int num_cmd = cmd->num_cmds;
    printf("#commands = %d\n", num_cmd);
    for (int i = 0; i < num_cmd; i++) {
        printf( "command #%d\n", i+1);
        int num_arg = cmd->cmds[i].num_args;
        printf("#arguments = %d\n", num_arg);
        for (int j = 0; j < num_arg; j++) {
            fprintf(stdout, "%s ", cmd->cmds[i].args[j]);
        }
        printf("\n");
        printf("input file: %s\n", cmd->file_in);
        printf("output file: %s\n", cmd->file_out);
    }
}