#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

void
_deflateprint(char *fmt, ...)
{
	char buf[256];
	va_list args;

	va_start(args, fmt);
	vseprint(buf, buf + sizeof buf, fmt, args);
	va_end(args);
	print("%s", buf);
}
