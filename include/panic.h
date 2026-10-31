/*
 * Lantest OS - kernel panic
 */
#pragma once

/* Print the message and halt the CPU permanently (interrupts off). */
void panic(const char *msg);
