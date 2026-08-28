#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rockiva/rockiva_common.h"
#include "rockiva/rockiva_det_api.h"

struct stub_handle {
	void *userdata;
	ROCKIVA_FrameReleaseCallback release_callback;
	ROCKIVA_DetectResultCallback detect_callback;
};

static int scenario_is(const char *name)
{
	const char *scenario = getenv("ROCKIVA_STUB_SCENARIO");

	return scenario && strcmp(scenario, name) == 0;
}

RockIvaRetCode ROCKIVA_GetVersion(const uint32_t max_len, char *version)
{
	if (!version || max_len == 0)
		return ROCKIVA_RET_NULL_PTR;
	if (scenario_is("version_fail"))
		return ROCKIVA_RET_FAIL;
	snprintf(version, max_len, "rockiva-host-stub");
	return ROCKIVA_RET_SUCCESS;
}

RockIvaRetCode ROCKIVA_Init(RockIvaHandle *handle, RockIvaWorkMode mode,
			    const RockIvaInitParam *param, void *userdata)
{
	struct stub_handle *stub;

	(void)mode;
	(void)param;
	if (scenario_is("init_fail"))
		return ROCKIVA_RET_FAIL;
	stub = calloc(1, sizeof(*stub));
	if (!stub)
		return ROCKIVA_RET_FAIL;
	stub->userdata = userdata;
	*handle = stub;
	return ROCKIVA_RET_SUCCESS;
}

RockIvaRetCode ROCKIVA_SetFrameReleaseCallback(
	RockIvaHandle handle, ROCKIVA_FrameReleaseCallback callback)
{
	struct stub_handle *stub = handle;

	if (!stub || !callback)
		return ROCKIVA_RET_NULL_PTR;
	if (scenario_is("release_callback_fail"))
		return ROCKIVA_RET_FAIL;
	stub->release_callback = callback;
	return ROCKIVA_RET_SUCCESS;
}

RockIvaRetCode ROCKIVA_DETECT_Init(
	RockIvaHandle handle, const RockIvaDetTaskParams *params,
	const ROCKIVA_DetectResultCallback result_callback)
{
	struct stub_handle *stub = handle;

	(void)params;
	if (!stub || !result_callback)
		return ROCKIVA_RET_NULL_PTR;
	if (scenario_is("detect_init_fail"))
		return ROCKIVA_RET_FAIL;
	stub->detect_callback = result_callback;
	return ROCKIVA_RET_SUCCESS;
}

RockIvaRetCode ROCKIVA_PushFrame(
	RockIvaHandle handle, const RockIvaImage *input,
	const RockIvaFrameExtraInfo *extra_info)
{
	struct stub_handle *stub = handle;
	RockIvaDetectResult result;
	RockIvaReleaseFrames released;

	(void)extra_info;
	if (!stub || !input)
		return ROCKIVA_RET_NULL_PTR;
	if (scenario_is("push_fail"))
		return ROCKIVA_RET_BUFFER_FULL;
	memset(&result, 0, sizeof(result));
	result.frameId = input->frameId;
	result.frame = *input;
	result.channelId = input->channelId;
	result.imageSize.width = input->info.width;
	result.imageSize.height = input->info.height;
	result.objNum = 1;
	result.objInfo[0].objId = 101;
	result.objInfo[0].frameId = input->frameId;
	result.objInfo[0].score = 90;
	result.objInfo[0].type = scenario_is("no_person")
					 ? ROCKIVA_OBJECT_TYPE_VEHICLE
					 : ROCKIVA_OBJECT_TYPE_PERSON;
	result.objInfo[0].state = input->frameId == 1 || scenario_is("no_tracking")
					 ? ROCKIVA_OBJECT_STATE_FIRST
					 : ROCKIVA_OBJECT_STATE_TRACKING;
	if (stub->detect_callback)
		stub->detect_callback(&result, ROCKIVA_SUCCESS, stub->userdata);
	memset(&released, 0, sizeof(released));
	released.channelId = input->channelId;
	released.count = 1;
	released.frames[0] = *input;
	if (stub->release_callback)
		stub->release_callback(&released, stub->userdata);
	return ROCKIVA_RET_SUCCESS;
}

RockIvaRetCode ROCKIVA_WaitFinish(RockIvaHandle handle, long frame_id,
				  int timeout_ms)
{
	(void)handle;
	(void)frame_id;
	(void)timeout_ms;
	return scenario_is("wait_fail") ? ROCKIVA_RET_FAIL : ROCKIVA_RET_SUCCESS;
}

RockIvaRetCode ROCKIVA_DETECT_Release(RockIvaHandle handle)
{
	(void)handle;
	return scenario_is("detect_release_fail") ? ROCKIVA_RET_FAIL
						   : ROCKIVA_RET_SUCCESS;
}

RockIvaRetCode ROCKIVA_Release(RockIvaHandle handle)
{
	free(handle);
	return scenario_is("release_fail") ? ROCKIVA_RET_FAIL : ROCKIVA_RET_SUCCESS;
}
