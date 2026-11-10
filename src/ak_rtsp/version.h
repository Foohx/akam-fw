#pragma once

/* Print one-line build identity: build number, timestamp, git ref. */
void print_build_info(void);

/* Short firmware identity ("b<build>-<git>"), stable for the life of the process. */
const char *firmware_version(void);
