#ifndef OMA_LOG_H
#define OMA_LOG_H

/* Daemon-side logging to stderr. The shell's Process captures stderr, so this
 * is what shows up in the journal. pjsip keeps its own, far noisier log in
 * ~/.local/state/omacall/pjsip.log. */

typedef enum {
    OMA_LOG_ERROR = 0,
    OMA_LOG_WARN  = 1,
    OMA_LOG_INFO  = 2,
    OMA_LOG_DEBUG = 3
} oma_log_level;

void oma_log_set_level(oma_log_level level);
void oma_log(oma_log_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define oma_err(...)  oma_log(OMA_LOG_ERROR, __VA_ARGS__)
#define oma_warn(...) oma_log(OMA_LOG_WARN, __VA_ARGS__)
#define oma_info(...) oma_log(OMA_LOG_INFO, __VA_ARGS__)
#define oma_dbg(...)  oma_log(OMA_LOG_DEBUG, __VA_ARGS__)

#endif /* OMA_LOG_H */
