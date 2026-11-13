#include <u.h>
#include <libc.h>

void
_deflateprint(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vfprint(2, fmt, args);
	va_end(args);
}
