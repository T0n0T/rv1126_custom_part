#ifndef ME_UTIL_H
#define ME_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	ME_LOG_ERROR = 0,
	ME_LOG_WARN = 1,
	ME_LOG_INFO = 2,
	ME_LOG_DEBUG = 3,
};

void me_log(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Strict decimal parse: no leading sign/whitespace, no trailing garbage,
 * value fits in uint32. Returns false on any violation. */
bool me_parse_u32_decimal(const char *s, uint32_t *out);

/* Returns true when s is a valid dotted-quad IPv4 address. */
bool me_valid_ipv4(const char *s);

void me_set_err(char *buf, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Trims ASCII whitespace in place. */
void me_trim(char *s);

#endif /* ME_UTIL_H */
