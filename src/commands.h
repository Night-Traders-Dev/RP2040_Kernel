#ifndef COMMANDS_H
#define COMMANDS_H

/*
 * dispatch() — parse and execute one shell input line.
 * All commands, including the PIO group, are registered internally.
 */
void dispatch(const char *input);

#endif