#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static oma_log_level current_level = OMA_LOG_INFO;

static const char *level_name(oma_log_level level)
{
    switch (level) {
    case OMA_LOG_ERROR: return "error";
    case OMA_LOG_WARN:  return "warn";
    case OMA_LOG_INFO:  return "info";
    case OMA_LOG_DEBUG: return "debug";
    }
    return "?";
}

void oma_log_set_level(oma_log_level level)
{
    current_level = level;
}

void oma_log(oma_log_level level, const char *fmt, ...)
{
    struct tm tm;
    time_t now;
    char stamp[32];
    va_list ap;

    if (level > current_level)
        return;

    now = time(NULL);
    if (localtime_r(&now, &tm) != NULL)
        strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    else
        stamp[0] = '\0';

    fprintf(stderr, "%s omacalld %-5s ", stamp, level_name(level));
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
