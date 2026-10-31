/*
 * Lantest OS - minimal signal support (M4-10)
 */
#pragma once

#include <types.h>
#include <x86_64/isr.h>

struct task;

#define SIGINT  2
#define SIGSEGV 11
#define SIGCONT 18
#define SIGTSTP 20
#define SIGPIPE 13

void signal_send(struct task *t, int sig);
void signal_deliver_pending(registers_t *regs);
