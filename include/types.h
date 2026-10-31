/*
 * Lantest OS - basic freestanding types
 *
 * The kernel is built with -nostdinc, so it cannot use <stdint.h>. These
 * fixed-width types are the kernel's own definitions.
 */
#pragma once

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long       u64;

typedef signed char         i8;
typedef signed short        i16;
typedef signed int          i32;
typedef signed long         i64;

typedef __SIZE_TYPE__       usize;
typedef __INTPTR_TYPE__     isize;

typedef _Bool               bool;

#define true  1
#define false 0

#define NULL ((void *)0)

/* Compile-time check without pulling in any header. */
#define static_assert(cond, msg) _Static_assert(cond, msg)
