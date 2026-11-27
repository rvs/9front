/* complete APE inttypes.h for git */
#include "/sys/include/ape/inttypes.h"
#ifndef PRIdMAX
#define PRIdMAX "lld"
#define PRIuMAX "llu"
#define PRIxMAX "llx"
#define PRIoMAX "llo"
#define PRIiMAX "lli"
#define PRIi64 "lli"
#define PRIX64 "llX"
#define PRIX32 "X"
#define PRIi32 "i"
#define SCNd64 "lld"
#define SCNu64 "llu"
#define SCNx64 "llx"
#define SCNuMAX "llu"
#endif
#ifndef PTRDIFF_MAX
#define PTRDIFF_MAX INT64_MAX
#define PTRDIFF_MIN INT64_MIN
#endif
char *getpass(const char *);
