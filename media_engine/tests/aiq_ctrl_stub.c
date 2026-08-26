/* Host-only stub for aiq_ctrl so media_engine can be built and smoke-tested
 * without the SDK RKAIQ headers/libraries. NOT part of the target build. */

#include "aiq/aiq_ctrl.h"

#include "common/me_errors.h"
#include "common/util.h"

int aiq_ctrl_start(AiqCtrl *a, int cam_id, const char *iq_dir,
                   const char *af_mode, char *err, size_t errsz)
{
	(void)cam_id;
	(void)iq_dir;
	(void)af_mode;
	a->error[0] = '\0';
	a->started = true;
	me_log(ME_LOG_INFO, "aiq_ctrl started (host stub)");
	return ME_ERR_OK;
}

void aiq_ctrl_stop(AiqCtrl *a)
{
	if (!a)
		return;
	a->ctx = NULL;
	a->started = false;
	me_log(ME_LOG_INFO, "aiq_ctrl stopped (host stub)");
}
