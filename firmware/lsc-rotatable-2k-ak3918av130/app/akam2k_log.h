/* Tiny logging and step-tracking interface shared by the akam2k_rtsp sources (implemented in akam2k_main.c). */
#ifndef AKAM2K_LOG_H
#define AKAM2K_LOG_H

/* printf-style line with a millisecond clock since start, flushed. Thread safe. */
void a2k_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* The step the run is in, printed by the alarm and crash handlers (a hang in a vendor call is bounded by alarm()). */
void a2k_set_step(const char *step);
/* One "mem <tag>: VmRSS, VmHWM, MemFree, MemAvailable" log line; MemAvailable in kB (-1 when unknown). */
void a2k_mem_report(const char *tag);
long a2k_mem_avail_kb(void);

#endif
