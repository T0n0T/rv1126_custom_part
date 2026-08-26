#ifndef ME_AIQ_CTRL_H
#define ME_AIQ_CTRL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
	bool started;
	void *ctx; /* rk_aiq_sys_ctx_t *, owned (aiq integration step) */
	char error[256]; /* last start failure detail */
} AiqCtrl;

int aiq_ctrl_start(AiqCtrl *a, int cam_id, const char *iq_dir,
                   const char *af_mode, char *err, size_t errsz);
void aiq_ctrl_stop(AiqCtrl *a);

#endif /* ME_AIQ_CTRL_H */
