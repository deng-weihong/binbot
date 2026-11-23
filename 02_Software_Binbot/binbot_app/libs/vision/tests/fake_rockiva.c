#include "rockiva_det_api.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    void *user_data;
    ROCKIVA_DetectResultCallback detect_callback;
    ROCKIVA_FrameReleaseCallback release_callback;
} fake_context_t;

RockIvaRetCode ROCKIVA_Init(RockIvaHandle *handle, RockIvaWorkMode mode,
                            const RockIvaInitParam *param, void *user_data)
{
    if(!handle || !param || mode != ROCKIVA_MODE_VIDEO ||
       param->detModel != ROCKIVA_DET_MODEL_PFP) {
        return ROCKIVA_RET_FAIL;
    }
    fake_context_t *context = calloc(1, sizeof(*context));
    if(!context) return ROCKIVA_RET_FAIL;
    context->user_data = user_data;
    *handle = context;
    return ROCKIVA_RET_SUCCESS;
}

RockIvaRetCode ROCKIVA_Release(RockIvaHandle handle)
{
    free(handle);
    return ROCKIVA_RET_SUCCESS;
}

RockIvaRetCode ROCKIVA_WaitFinish(RockIvaHandle handle, long frame_id, int timeout_ms)
{
    (void)frame_id;
    (void)timeout_ms;
    return handle ? ROCKIVA_RET_SUCCESS : ROCKIVA_RET_INVALID_HANDLE;
}

RockIvaRetCode ROCKIVA_SetFrameReleaseCallback(
    RockIvaHandle handle, ROCKIVA_FrameReleaseCallback callback)
{
    fake_context_t *context = handle;
    if(!context || !callback) return ROCKIVA_RET_NULL_PTR;
    context->release_callback = callback;
    return ROCKIVA_RET_SUCCESS;
}

RockIvaRetCode ROCKIVA_IMAGE_AllocMem(RockIvaImage *image, RockIvaMemType type)
{
    if(!image || type != ROCKIVA_MEM_TYPE_DMA ||
       image->info.format != ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12) {
        return ROCKIVA_RET_FAIL;
    }
    size_t size = (size_t)image->info.width * image->info.height * 3U / 2U;
    image->dataAddr = calloc(1, size);
    if(!image->dataAddr) return ROCKIVA_RET_FAIL;
    image->size = (uint32_t)size;
    image->dataFd = 77;
    return ROCKIVA_RET_SUCCESS;
}

RockIvaRetCode ROCKIVA_IMAGE_FreeMem(RockIvaImage *image)
{
    if(!image || !image->dataAddr || image->dataFd < 0) return ROCKIVA_RET_FAIL;
    free(image->dataAddr);
    image->dataAddr = NULL;
    image->dataFd = -1;
    return ROCKIVA_RET_SUCCESS;
}

RockIvaRetCode ROCKIVA_DETECT_Init(
    RockIvaHandle handle, const RockIvaDetTaskParams *params,
    ROCKIVA_DetectResultCallback callback)
{
    fake_context_t *context = handle;
    if(!context || !params || !callback ||
       !(params->detObjectType & ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_FACE))) {
        return ROCKIVA_RET_FAIL;
    }
    context->detect_callback = callback;
    return ROCKIVA_RET_SUCCESS;
}

RockIvaRetCode ROCKIVA_DETECT_Release(RockIvaHandle handle)
{
    return handle ? ROCKIVA_RET_SUCCESS : ROCKIVA_RET_INVALID_HANDLE;
}

RockIvaRetCode ROCKIVA_PushFrame(RockIvaHandle handle, const RockIvaImage *image,
                                 const RockIvaFrameExtraInfo *extra)
{
    (void)extra;
    fake_context_t *context = handle;
    if(!context || !context->detect_callback || !context->release_callback || !image ||
       image->info.format != ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12 || !image->dataAddr ||
       image->dataFd < 0 || !image->extData || image->dataAddr[0] != 1 ||
       image->dataAddr[(size_t)image->info.width * image->info.height] != 2) {
        return ROCKIVA_RET_FAIL;
    }

    RockIvaDetectResult result;
    memset(&result, 0, sizeof(result));
    result.frameId = image->frameId;
    result.frame = *image;
    result.objNum = 1;
    result.objInfo[0].objId = 42;
    result.objInfo[0].frameId = image->frameId;
    result.objInfo[0].score = 88;
    result.objInfo[0].type = ROCKIVA_OBJECT_TYPE_FACE;
    result.objInfo[0].rect.topLeft.x = 1000;
    result.objInfo[0].rect.topLeft.y = 2000;
    result.objInfo[0].rect.bottomRight.x = 6000;
    result.objInfo[0].rect.bottomRight.y = 8000;
    context->detect_callback(&result, ROCKIVA_SUCCESS, context->user_data);

    RockIvaReleaseFrames released;
    memset(&released, 0, sizeof(released));
    released.count = 1;
    released.frames[0] = *image;
    context->release_callback(&released, context->user_data);
    return ROCKIVA_RET_SUCCESS;
}
