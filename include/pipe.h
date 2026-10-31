/*
 * Lantest OS - pipe IPC (M4-8)
 */
#pragma once

#include <types.h>
#include <fs.h>

struct pipe;

struct pipe *pipe_alloc(void);
struct file *pipe_make_file(struct pipe *p, int write);
