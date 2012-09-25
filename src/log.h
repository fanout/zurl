#ifndef LOG_H
#define LOG_H

// really simply logging stuff

#define LOG_LEVEL_ERROR   0
#define LOG_LEVEL_WARNING 1
#define LOG_LEVEL_INFO    2
#define LOG_LEVEL_DEBUG   3

void log_setOutputLevel(int level);

void log_error(const char *fmt, ...);
void log_warning(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_debug(const char *fmt, ...);

#endif
