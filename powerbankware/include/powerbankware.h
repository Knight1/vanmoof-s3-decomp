#ifndef POWERBANKWARE_H
#define POWERBANKWARE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * powerbankware — VanMoof PowerBank application (STM32F091xC, Cortex-M0).
 *
 * Seed header for an in-progress decomp. Prototypes are added per function
 * as src/ files land (mirroring batteryware's batteryware.h). The firmware
 * is a BMS sibling of batteryware (shared FEDL5236 core) plus a power-path
 * output stage (bypass FET, DAC-regulated Vout, charger/load detection).
 *
 * Startup chain (verified): Reset_Handler -> SystemInit -> __libc_init_array
 * -> main. main() runs the mode-gated state-machine super-loop.
 */

void Reset_Handler(void);
void SystemInit(void);
void __libc_init_array_lite(void);
int  main(void);

#endif /* POWERBANKWARE_H */
