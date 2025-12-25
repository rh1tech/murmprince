/**
 * Stub for murmdoom_log.h
 * Provides logging macros that emu8950 needs
 */
#ifndef MURMDOOM_LOG_H
#define MURMDOOM_LOG_H

// Disable logging for emu8950 in Prince of Persia
#define I_Printf(...) ((void)0)
#define I_Error(...) ((void)0)
#define MURMDOOM_WARN(...) ((void)0)

#endif // MURMDOOM_LOG_H
