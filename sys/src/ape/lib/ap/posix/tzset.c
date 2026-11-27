#include <time.h>
#include <string.h>

extern void _tzinfo(int, char**, char**, long*, long*);

char *tzname[2];
long timezone;
long altzone;
int daylight;

void
tzset(void)
{
	long st, dl;

	_tzinfo(1, &tzname[0], &tzname[1], &st, &dl);
	timezone = -st;
	altzone = -dl;
	daylight = strcmp(tzname[0], tzname[1]) != 0;
}
