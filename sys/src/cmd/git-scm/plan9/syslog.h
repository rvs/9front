/* Plan 9 APE has no syslog */
#define LOG_PID 0
#define LOG_DAEMON 0
#define LOG_ERR 3
#define LOG_INFO 6
#define openlog(i, o, f)
#define syslog(p, ...)
