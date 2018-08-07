#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

int cmd_exit(struct tokens *tokens);
int cmd_help(struct tokens *tokens);
int cmd_pwd(struct tokens *tokens);
int cmd_cd(struct tokens *tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens *tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
  {cmd_pwd, "pwd", "print working directory"},
  {cmd_cd, "cd", "change directory"},
  {cmd_help, "?", "show this help menu"},
  {cmd_exit, "exit", "exit the command shell"},
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens *tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens *tokens) {
  exit(0);
}

/* Changes to new directory */
int cmd_cd(struct tokens *tokens) {
  char* dir = tokens_get_token(tokens, 1);
  chdir(dir);
  return 0;
}

/* Print working directory */
int cmd_pwd(unused struct tokens *tokens) {
  char cwd[1024];
  if (getcwd(cwd, sizeof(cwd)) != NULL)
    printf("%s\n", cwd);
  else
    perror("getcwd() error");
  return 0;
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

/* Get PATH env variable, fill array of strings
 * return value = number of paths */
int get_env_paths(char** paths) {
  char *pathvar = strdup(getenv("PATH"));
  int i = 0;
  char *p = strtok(pathvar, ":");
  
  while (p != NULL) {
    paths[i++] = p;
    p = strtok(NULL, ":");
  }
  return i;
}

/*From a path formatted like "/usr/bin/wc", get the "wc"*/
char* get_last_path_token(char *path) {
  char *tok, *cmd, *ptr;
  char *pathcp = malloc(strlen(path));
  strcpy(pathcp, path);
  tok = strtok_r(pathcp, "/", &ptr);
  while (tok != NULL) {
    tok = strtok_r(NULL, "/", &ptr);
    if (tok == NULL) break;
    cmd = tok;
  }
  return cmd;
}

/*From a list of paths and a command, return the first combination of*/
/*<path>/<cmd> that exists*/
char* get_first_abs_path(char **paths, int env_path_len, char* cmd) {

  for(int i = 0; i < env_path_len; i++) {

    char *full_path = malloc(strlen(paths[i]) + strlen(cmd) + 1);
    /*Ensures memory is an empty string*/
    full_path[0] = '\0';

    strcat(full_path, paths[i]);
    strcat(full_path, "/");
    strcat(full_path, cmd);
  
    /*If file exists, return it*/
    if (access(full_path, F_OK) != -1) {
      return full_path;
    }
  }

  return NULL;
}

/* builds an arg list from tokens_get_tokens() and skips the redirection 
 * (<, >) chars, also fills input/output redirection outparams*/
char** get_args(struct tokens *tokens,
                int tok_len,
                char **args,
                char *redir_syn,
                char *redir_target) {
  int i;
  for (i = 0; i < tok_len; i++){
    char* token = tokens_get_token(tokens, i);
    if (strcmp(">",token)==0 || strcmp("<",token)==0) {
      redir_syn[0] = '\0';
      strcat(redir_syn, token);
      break;
    }
    args[i] = token;
  }
  /*execv args needs to be null terminated*/
  args[i] = NULL;
  /*last token must be redir_target bc we broke out of for loop*/
  if (i < tok_len) {
    redir_target[0] = '\0';
    strcat(redir_target, tokens_get_token(tokens, i+1));
  }
  return args;
}

/*Run a given command with arguments*/
void run_cmd(struct tokens *tokens){

  int tok_len = tokens_get_length(tokens);

  if (tok_len > 0){

    char *cmd = tokens_get_token(tokens, 0);
    char *paths[32];
    int env_path_len = get_env_paths(paths);
    char *abs_cmd = get_first_abs_path(paths, env_path_len, cmd);

    if (abs_cmd != NULL) {

      char redir_syn[1];
      char redir_target[128];
      char *args[tok_len]; 
      get_args(tokens, tok_len, args, redir_syn, redir_target);
      args[0] = get_last_path_token(abs_cmd);

      /*printf("%s\n", redir_syn);*/
      /*printf("%s\n", redir_target);*/

      redir_syn[0] = '\0';

      pid_t pid = fork();
      if (pid == 0) {
        execv(abs_cmd, args);
        exit(EXIT_SUCCESS);
      } else if (pid == -1) {
        /*failed to fork()*/
      } else {
        int status;
        wait(&status);
      }
    } else {
      fprintf(stderr, "%s: command not found\n", cmd);
    }
  }
}

int main(unused int argc, unused char *argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens *tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      run_cmd(tokens);
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
