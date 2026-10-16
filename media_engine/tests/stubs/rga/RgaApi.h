#ifndef ME_TEST_RGA_API_H
#define ME_TEST_RGA_API_H

#include <stdlib.h>

typedef struct {
	int unused;
} rga_rect_t;

typedef struct {
	int fd;
	void *virAddr;
	int mmuFlag;
	int rotation;
	rga_rect_t rect;
} rga_info_t;

#define c_RkRgaInit() (abort(), -1)
#define c_RkRgaBlit(...) (abort(), -1)
#define rga_set_rect(...) abort()

#endif
