#include "common/util.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *level_name(int level)
{
	switch (level) {
	case ME_LOG_ERROR:
		return "ERROR";
	case ME_LOG_WARN:
		return "WARN";
	case ME_LOG_INFO:
		return "INFO";
	case ME_LOG_DEBUG:
		return "DEBUG";
	default:
		return "?";
	}
}

void me_log(int level, const char *fmt, ...)
{
	struct timeval tv;
	struct tm tm;
	char stamp[64];
	va_list ap;

	gettimeofday(&tv, NULL);
	localtime_r(&tv.tv_sec, &tm);
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

	fprintf(stderr, "[%s.%03ld] %-5s media_engine: ", stamp,
	        (long)(tv.tv_usec / 1000), level_name(level));
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

bool me_parse_u32_decimal(const char *s, uint32_t *out)
{
	unsigned long long v;
	char *end = NULL;

	if (!s || !*s || *s == '+' || *s == '-')
		return false;
	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno || !end || *end != '\0' || v > UINT32_MAX)
		return false;
	if (out)
		*out = (uint32_t)v;
	return true;
}

bool me_valid_ipv4(const char *s)
{
	struct in_addr addr;

	return s && *s && inet_pton(AF_INET, s, &addr) == 1;
}

void me_set_err(char *buf, size_t cap, const char *fmt, ...)
{
	va_list ap;

	if (!buf || cap == 0)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, cap, fmt, ap);
	va_end(ap);
}

void me_trim(char *s)
{
	char *p;
	char *end;

	if (!s)
		return;
	p = s;
	while (*p && isspace((unsigned char)*p))
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
}
