#include "aiq/aiq_ctrl.h"

#include "common/me_errors.h"
#include "common/util.h"

#include <rk_aiq_user_api2_imgproc.h>
#include <rk_aiq_user_api2_sysctl.h>
#include <rk_mpi_vi.h>

#include <stdio.h>
#include <string.h>

static int configure_focus(rk_aiq_sys_ctx_t *ctx, const char *mode, char *err,
                           size_t errsz)
{
	opMode_t aiq_mode;
	XCamReturn ret;

	if (!mode || !strcmp(mode, "off"))
		return 0;
	if (!strcmp(mode, "auto")) {
		aiq_mode = OP_AUTO;
	} else if (!strcmp(mode, "semi-auto")) {
		aiq_mode = OP_SEMI_AUTO;
	} else if (!strcmp(mode, "manual")) {
		aiq_mode = OP_MANUAL;
	} else {
		me_set_err(err, errsz, "invalid focus mode: %s", mode);
		return -1;
	}

	ret = rk_aiq_uapi2_setFocusMode(ctx, aiq_mode);
	me_log(ME_LOG_INFO, "aiq focus mode %s ret=%d", mode, ret);
	if (ret != XCAM_RETURN_NO_ERROR) {
		me_set_err(err, errsz, "rk_aiq setFocusMode failed: %d", ret);
		return -1;
	}
	if (!strcmp(mode, "semi-auto")) {
		ret = rk_aiq_uapi2_oneshotFocus(ctx);
		me_log(ME_LOG_INFO, "aiq oneshot focus ret=%d", ret);
		if (ret != XCAM_RETURN_NO_ERROR) {
			me_set_err(err, errsz, "rk_aiq oneshotFocus failed: %d", ret);
			return -1;
		}
	}
	return 0;
}

int aiq_ctrl_start(AiqCtrl *a, int cam_id, const char *iq_dir,
                   const char *af_mode, char *err, size_t errsz)
{
	rk_aiq_static_info_t static_info;
	rk_aiq_sys_ctx_t *ctx = NULL;
	XCamReturn ret;

	if (!a)
		return ME_ERR_MEDIA;
	a->error[0] = '\0';

	memset(&static_info, 0, sizeof(static_info));
	ret = rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(cam_id, &static_info);
	if (ret != XCAM_RETURN_NO_ERROR) {
		snprintf(a->error, sizeof(a->error),
		         "rk_aiq enum static metas failed: %d", ret);
		me_log(ME_LOG_ERROR, "%s", a->error);
		me_set_err(err, errsz, "%s", a->error);
		return ME_ERR_MEDIA;
	}
	if (static_info.sensor_info.phyId == -1 ||
	    static_info.sensor_info.sensor_name[0] == '\0') {
		snprintf(a->error, sizeof(a->error), "no sensor found for camera %d",
		         cam_id);
		me_log(ME_LOG_ERROR, "%s", a->error);
		me_set_err(err, errsz, "%s", a->error);
		return ME_ERR_MEDIA;
	}

	me_log(ME_LOG_INFO, "aiq camera %d sensor: %s, iq_dir: %s", cam_id,
	       static_info.sensor_info.sensor_name, iq_dir);

	ret = rk_aiq_uapi2_sysctl_preInit_scene(static_info.sensor_info.sensor_name,
	                                        "normal", "day");
	if (ret != XCAM_RETURN_NO_ERROR)
		me_log(ME_LOG_WARN, "aiq preInit_scene failed: %d", ret);

	ctx = rk_aiq_uapi2_sysctl_init(static_info.sensor_info.sensor_name, iq_dir,
	                               NULL, NULL);
	if (!ctx) {
		snprintf(a->error, sizeof(a->error), "rk_aiq sysctl init failed");
		me_log(ME_LOG_ERROR, "%s", a->error);
		me_set_err(err, errsz, "%s", a->error);
		return ME_ERR_MEDIA;
	}

	ret = rk_aiq_uapi2_sysctl_prepare(ctx, 0, 0, RK_AIQ_WORKING_MODE_NORMAL);
	if (ret != XCAM_RETURN_NO_ERROR) {
		snprintf(a->error, sizeof(a->error), "rk_aiq sysctl prepare failed: %d",
		         ret);
		me_log(ME_LOG_ERROR, "%s", a->error);
		rk_aiq_uapi2_sysctl_deinit(ctx);
		me_set_err(err, errsz, "%s", a->error);
		return ME_ERR_MEDIA;
	}

	if (rk_aiq_uapi2_judgeNeedRawPipeCtrl(ctx)) {
		RK_MPI_VI_CtrlRawStream(RK_FALSE);
		me_log(ME_LOG_INFO, "aiq raw stream control: rkaiq");
	} else {
		RK_MPI_VI_CtrlRawStream(RK_TRUE);
		me_log(ME_LOG_INFO, "aiq raw stream control: rockit");
	}

	ret = rk_aiq_uapi2_sysctl_start(ctx);
	if (ret != XCAM_RETURN_NO_ERROR) {
		snprintf(a->error, sizeof(a->error), "rk_aiq sysctl start failed: %d",
		         ret);
		me_log(ME_LOG_ERROR, "%s", a->error);
		rk_aiq_uapi2_sysctl_deinit(ctx);
		me_set_err(err, errsz, "%s", a->error);
		return ME_ERR_MEDIA;
	}

	if (configure_focus(ctx, af_mode, err, errsz) != 0) {
		snprintf(a->error, sizeof(a->error), "focus setup failed: %s",
		         err && *err ? err : "unknown");
		rk_aiq_uapi2_sysctl_stop(ctx, false);
		rk_aiq_uapi2_sysctl_deinit(ctx);
		return ME_ERR_MEDIA;
	}

	a->ctx = ctx;
	a->started = true;
	me_log(ME_LOG_INFO, "aiq started (cam %d, af=%s)", cam_id,
	       af_mode ? af_mode : "off");
	return ME_ERR_OK;
}

void aiq_ctrl_stop(AiqCtrl *a)
{
	if (!a)
		return;
	if (a->ctx) {
		rk_aiq_uapi2_sysctl_stop((rk_aiq_sys_ctx_t *)a->ctx, false);
		rk_aiq_uapi2_sysctl_deinit((rk_aiq_sys_ctx_t *)a->ctx);
		a->ctx = NULL;
	}
	a->started = false;
	me_log(ME_LOG_INFO, "aiq stopped");
}
