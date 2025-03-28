/* encvt.c

   Copyright (c) 2003-2025 HandBrake Team
   This file is part of the HandBrake source code
   Homepage: <http://handbrake.fr/>.
   It may be used under the terms of the GNU General Public License v2.
   For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#include <VideoToolbox/VideoToolbox.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include "libavutil/avutil.h"

#include "handbrake/handbrake.h"
#include "handbrake/dovi_common.h"
#include "handbrake/hdr10plus.h"
#include "handbrake/nal_units.h"
#include "handbrake/extradata.h"
#include "handbrake/bitstream.h"

#include "vt_common.h"
#include "cv_utils.h"

int  encvt_init(hb_work_object_t *, hb_job_t *);
int  encvt_work(hb_work_object_t *, hb_buffer_t **, hb_buffer_t **);
void encvt_close(hb_work_object_t *);

hb_work_object_t hb_encvt =
{
    WORK_ENCVT,
    "VideoToolbox encoder (Apple)",
    encvt_init,
    encvt_work,
    encvt_close
};

#define FRAME_INFO_SIZE 1024
#define FRAME_INFO_MASK (FRAME_INFO_SIZE - 1)

struct hb_work_private_s
{
    hb_job_t *job;

    CMFormatDescriptionRef  format;
    VTCompressionSessionRef session;

    CMSimpleQueueRef    queue;
    hb_chapter_queue_t *chapter_queue;

    // DTS calculation
    int     frameno_in;
    int     frameno_out;
    int64_t dts_delay;

    struct
    {
        int64_t     start;
    } frame_info[FRAME_INFO_SIZE];

    // Multipass
    VTMultiPassStorageRef passStorage;
    CMItemCount           timeRangeCount;
    const CMTimeRange    *timeRangeArray;
    int                   remainingPasses;

    uint8 nal_length_size;

    struct hb_vt_param
    {
        CMVideoCodecType codec;
        uint64_t registryID;

        OSType inputPixFmt;
        OSType encoderPixFmt;

        int32_t timescale;

        double quality;
        int averageBitRate;
        double expectedFrameRate;

        int profile;
        CFStringRef profileLevel;

        int maxAllowedFrameQP;
        int minAllowedFrameQP;
        int maxReferenceBufferCount;
        int maxFrameDelayCount;
        int maxKeyFrameInterval;
        int lookAheadFrameCount;
        CFBooleanRef allowFrameReordering;
        CFBooleanRef allowTemporalCompression;
        CFBooleanRef disableSpatialAdaptiveQP;
        CFBooleanRef prioritizeEncodingSpeedOverQuality;
        CFBooleanRef preserveDynamicHDRMetadata;
        struct
        {
            int maxrate;
            int bufsize;
        }
        vbv;
        struct
        {
            int prim;
            int matrix;
            int transfer;
            int chromaLocation;
            CFDataRef masteringDisplay;
            CFDataRef contentLightLevel;
            CFDataRef ambientViewingEnviroment;
        }
        color;
        SInt32 width;
        SInt32 height;
        struct
        {
            SInt32 num;
            SInt32 den;
        }
        par;
        enum
        {
            HB_VT_FIELDORDER_PROGRESSIVE = 0,
            HB_VT_FIELDORDER_TFF,
            HB_VT_FIELDORDER_BFF,
        }
        fieldDetail;
        struct
        {
            CFStringRef entropyMode;
            int maxSliceBytes;
        }
        h264;
    }
    settings;

    CFDictionaryRef attachments;
};

void hb_vt_param_default(struct hb_vt_param *param)
{
    param->quality                  = -1;
    param->vbv.maxrate              =  0;
    param->vbv.bufsize              =  0;
    param->maxAllowedFrameQP        = -1;
    param->minAllowedFrameQP        = -1;
    param->maxReferenceBufferCount  = -1;
    param->maxFrameDelayCount       = kVTUnlimitedFrameDelayCount;
    param->lookAheadFrameCount      = -1;
    param->allowFrameReordering     = kCFBooleanTrue;
    param->allowTemporalCompression = kCFBooleanTrue;
    param->disableSpatialAdaptiveQP = kCFBooleanFalse;
    param->prioritizeEncodingSpeedOverQuality = kCFBooleanFalse;
    param->preserveDynamicHDRMetadata         = kCFBooleanFalse;
    param->fieldDetail              = HB_VT_FIELDORDER_PROGRESSIVE;
}

// Used to pass the compression
// session to the next job
typedef struct vt_interjob_s
{
    VTCompressionSessionRef session;
    VTMultiPassStorageRef   passStorage;
    CMSimpleQueueRef        queue;
    CMFormatDescriptionRef  format;
    int                     areBframes;
} vt_interjob_t;

enum
{
    HB_VT_H264_PROFILE_BASELINE = 0,
    HB_VT_H264_PROFILE_MAIN,
    HB_VT_H264_PROFILE_HIGH,
    HB_VT_H264_PROFILE_NB,
};

static struct
{
    const char *name;
    const CFStringRef level[HB_VT_H264_PROFILE_NB];
}
hb_vt_h264_levels[] =
{
    { "auto", { CFSTR("H264_Baseline_AutoLevel"), CFSTR("H264_Main_AutoLevel"), CFSTR("H264_High_AutoLevel"), }, },
    { "1.3",  { CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), }, },
    { "3.0",  { CFSTR("H264_Baseline_3_0"), CFSTR("H264_Main_3_0"    ), CFSTR("H264_High_3_0"    ), }, },
    { "3.1",  { CFSTR("H264_Baseline_3_1"), CFSTR("H264_Main_3_1"    ), CFSTR("H264_High_3_1"    ), }, },
    { "3.2",  { CFSTR("H264_Baseline_3_2"), CFSTR("H264_Main_3_2"    ), CFSTR("H264_High_3_2"    ), }, },
    { "4.0",  { CFSTR("H264_Baseline_4_0"), CFSTR("H264_Main_4_0"    ), CFSTR("H264_High_4_0"    ), }, },
    { "4.1",  { CFSTR("H264_Baseline_4_1"), CFSTR("H264_Main_4_1"    ), CFSTR("H264_High_4_1"    ), }, },
    { "4.2",  { CFSTR("H264_Baseline_4_2"), CFSTR("H264_Main_4_2"    ), CFSTR("H264_High_4_2"    ), }, },
    { "5.0",  { CFSTR("H264_Baseline_5_0"), CFSTR("H264_Main_5_0"    ), CFSTR("H264_High_5_0"    ), }, },
    { "5.1",  { CFSTR("H264_Baseline_5_1"), CFSTR("H264_Main_5_1"    ), CFSTR("H264_High_5_1"    ), }, },
    { "5.2",  { CFSTR("H264_Baseline_5_2"), CFSTR("H264_Main_5_2"    ), CFSTR("H264_High_5_2"    ), }, },
    { NULL,   { NULL,                       NULL,                       NULL,                       }, },
};

enum
{
    HB_VT_H265_PROFILE_MAIN = 0,
    HB_VT_H265_PROFILE_MAIN_10,
    HB_VT_H265_PROFILE_MAIN_422_10,
    HB_VT_H265_PROFILE_NB,
};

static struct
{
    const char *name;
    const CFStringRef level[HB_VT_H265_PROFILE_NB];
}
hb_vt_h265_levels[] =
{
    { "auto", { CFSTR("HEVC_Main_AutoLevel"), CFSTR("HEVC_Main10_AutoLevel"), CFSTR("HEVC_Main42210_AutoLevel") } }
};

static void hb_vt_save_frame_info(hb_work_private_t *pv, hb_buffer_t *in)
{
    int i = pv->frameno_in & FRAME_INFO_MASK;
    pv->frame_info[i].start = in->s.start;
}

static int64_t hb_vt_get_frame_start(hb_work_private_t *pv, int64_t frameno)
{
    int i = frameno & FRAME_INFO_MASK;
    return pv->frame_info[i].start;
}

static void hb_vt_compute_dts_offset(hb_work_private_t *pv, hb_buffer_t *buf)
{
    if (pv->job->areBframes &&
        pv->frameno_in == pv->job->areBframes)
    {
        pv->dts_delay = buf->s.start;
        pv->job->init_delay = pv->dts_delay;
    }
}

static void hb_vt_check_result(OSStatus err, CFStringRef propertyKey)
{
    if (err != noErr)
    {
        static const int VAL_BUF_LEN = 256;
        char valBuf[VAL_BUF_LEN];

        Boolean haveStr = CFStringGetCString(propertyKey,
                                             valBuf,
                                             VAL_BUF_LEN,
                                             kCFStringEncodingUTF8);
        if (haveStr)
        {
            hb_log("VTSessionSetProperty: %s failed (%d)", valBuf, err);
        }
        else
        {
            hb_log("VTSessionSetProperty: failed (%d)", err);
        }
    }
}

static OSStatus hb_vt_set_property(VTSessionRef session, CFStringRef propertyKey, CFTypeRef propertyValue)
{
    OSStatus err = VTSessionSetProperty(session, propertyKey, propertyValue);
    hb_vt_check_result(err, propertyKey);
    return err;
}

static int hb_vt_get_nal_length_size(CMSampleBufferRef sampleBuffer, CMVideoCodecType codec)
{
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sampleBuffer);
    int isize = 0;

    if (format)
    {
        if (codec == kCMVideoCodecType_H264)
        {
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, NULL, NULL, NULL, &isize);
        }
        else if (codec == kCMVideoCodecType_HEVC)
        {
            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, 0, NULL, NULL, NULL, &isize);
        }
    }

    return isize;
}

static void hb_vt_add_dynamic_hdr_metadata(CMSampleBufferRef sampleBuffer, hb_buffer_t *buf)
{
    for (int i = 0; i < buf->nb_side_data; i++)
    {
        const AVFrameSideData *side_data = buf->side_data[i];
        if (side_data->type == AV_FRAME_DATA_DYNAMIC_HDR_PLUS)
        {
            uint8_t *payload = NULL;
            uint32_t playload_size = 0;

            hb_dynamic_hdr10_plus_to_itu_t_t35((AVDynamicHDRPlus *)side_data->data, &payload, &playload_size);
            if (!playload_size)
            {
                continue;
            }

            CFDataRef data = CFDataCreate(kCFAllocatorDefault, payload, playload_size);
            if (data)
            {
                CMSetAttachment(sampleBuffer, CFSTR("HB_HDR_PLUS"), data, kCVAttachmentMode_ShouldPropagate);
                CFRelease(data);
            }
            av_freep(&payload);
        }
        if (side_data->type == AV_FRAME_DATA_DOVI_RPU_BUFFER)
        {
            CFDataRef data = CFDataCreate(kCFAllocatorDefault, side_data->data, side_data->size);
            if (data)
            {
                CMSetAttachment(sampleBuffer, CFSTR("HB_DOVI_RPU"), data, kCVAttachmentMode_ShouldPropagate);
                CFRelease(data);
            }
        }
    }
}

static CFDataRef hb_vt_mastering_display_xlat(hb_mastering_display_metadata_t mastering)
{
    CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 24);

    const int chromaDen = 50000;
    const int lumaDen = 10000;

    uint16_t display_primaries_gx = CFSwapInt16HostToBig(hb_rescale_rational(mastering.display_primaries[1][0], chromaDen));
    uint16_t display_primaries_gy = CFSwapInt16HostToBig(hb_rescale_rational(mastering.display_primaries[1][1], chromaDen));
    uint16_t display_primaries_bx = CFSwapInt16HostToBig(hb_rescale_rational(mastering.display_primaries[2][0], chromaDen));
    uint16_t display_primaries_by = CFSwapInt16HostToBig(hb_rescale_rational(mastering.display_primaries[2][1], chromaDen));
    uint16_t display_primaries_rx = CFSwapInt16HostToBig(hb_rescale_rational(mastering.display_primaries[0][0], chromaDen));
    uint16_t display_primaries_ry = CFSwapInt16HostToBig(hb_rescale_rational(mastering.display_primaries[0][1], chromaDen));

    uint16_t white_point_x = CFSwapInt16HostToBig(hb_rescale_rational(mastering.white_point[0], chromaDen));
    uint16_t white_point_y = CFSwapInt16HostToBig(hb_rescale_rational(mastering.white_point[1], chromaDen));

    uint32_t max_display_mastering_luminance = CFSwapInt32HostToBig(hb_rescale_rational(mastering.max_luminance, lumaDen));
    uint32_t min_display_mastering_luminance = CFSwapInt32HostToBig(hb_rescale_rational(mastering.min_luminance, lumaDen));

    CFDataAppendBytes(data, (UInt8 *)&display_primaries_gx, 2);
    CFDataAppendBytes(data, (UInt8 *)&display_primaries_gy, 2);
    CFDataAppendBytes(data, (UInt8 *)&display_primaries_bx, 2);
    CFDataAppendBytes(data, (UInt8 *)&display_primaries_by, 2);
    CFDataAppendBytes(data, (UInt8 *)&display_primaries_rx, 2);
    CFDataAppendBytes(data, (UInt8 *)&display_primaries_ry, 2);

    CFDataAppendBytes(data, (UInt8 *)&white_point_x, 2);
    CFDataAppendBytes(data, (UInt8 *)&white_point_y, 2);

    CFDataAppendBytes(data, (UInt8 *)&max_display_mastering_luminance, 4);
    CFDataAppendBytes(data, (UInt8 *)&min_display_mastering_luminance, 4);

    return data;
}

static CFDataRef hb_vt_content_light_level_xlat(hb_content_light_metadata_t coll)
{
    CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 4);

    uint16_t MaxCLL = CFSwapInt16HostToBig(coll.max_cll);
    uint16_t MaxFALL =  CFSwapInt16HostToBig(coll.max_fall);

    CFDataAppendBytes(data, (UInt8 *)&MaxCLL, 2);
    CFDataAppendBytes(data, (UInt8 *)&MaxFALL, 2);

    return data;
}

static CFDataRef hb_vt_ambient_viewing_enviroment_xlat(hb_ambient_viewing_environment_metadata_t ambient)
{
    CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 8);

    uint32_t ambient_illuminance = CFSwapInt32HostToBig(hb_rescale_rational(ambient.ambient_illuminance, 10000));
    uint16_t ambient_light_x =  CFSwapInt16HostToBig(hb_rescale_rational(ambient.ambient_light_x, 50000));
    uint16_t ambient_light_y =  CFSwapInt16HostToBig(hb_rescale_rational(ambient.ambient_light_y, 50000));

    CFDataAppendBytes(data, (UInt8 *)&ambient_illuminance, 4);
    CFDataAppendBytes(data, (UInt8 *)&ambient_light_x, 2);
    CFDataAppendBytes(data, (UInt8 *)&ambient_light_y, 2);

    return data;
}

static OSType hb_vt_encoder_pixel_format_xlat(int vcodec, int profile, int color_range)
{
    int pix_fmt = AV_PIX_FMT_NV12;

    switch (vcodec)
    {
        case HB_VCODEC_VT_H264:
        case HB_VCODEC_VT_H265:
            pix_fmt = hb_vt_get_best_pix_fmt(vcodec, "auto");
            break;
        case HB_VCODEC_VT_H265_10BIT:
            switch (profile)
            {
                case HB_VT_H265_PROFILE_MAIN_10:
                    pix_fmt = hb_vt_get_best_pix_fmt(vcodec, "main-10");
                case HB_VT_H265_PROFILE_MAIN_422_10:
                    pix_fmt = hb_vt_get_best_pix_fmt(vcodec, "main422-10");
            }
            break;
        default:
            hb_log("encvt_Init: unknown codec");
    }

    return hb_cv_get_pixel_format(pix_fmt, color_range);
}

static CFDictionaryRef hb_vt_attachments_xlat(hb_job_t *job)
{
    CFMutableDictionaryRef mutable_attachments = CFDictionaryCreateMutable(NULL, 0,
                                                                   &kCFTypeDictionaryKeyCallBacks,
                                                                   &kCFTypeDictionaryValueCallBacks);
    hb_cv_add_color_tag(mutable_attachments,
                        job->color_prim, job->color_transfer,
                        job->color_matrix, job->chroma_location);

    CFDictionaryRef attachments = CFDictionaryCreateCopy(NULL, mutable_attachments);
    CFRelease(mutable_attachments);
    return attachments;
}

static int hb_vt_settings_xlat(hb_work_private_t *pv, hb_job_t *job)
{
    // Set global default values.
    hb_vt_param_default(&pv->settings);

    pv->settings.codec       = job->vcodec == HB_VCODEC_VT_H264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC;
    pv->settings.inputPixFmt = hb_cv_get_pixel_format(job->output_pix_fmt, job->color_range);
    pv->settings.timescale   = 90000;

    // Set the preset
    if (job->encoder_preset != NULL && *job->encoder_preset != '\0')
    {
        if (!strcasecmp(job->encoder_preset, "speed"))
        {
            pv->settings.prioritizeEncodingSpeedOverQuality = kCFBooleanTrue;
        }
    }

    // Set the profile and level before initializing the session
    if (job->encoder_profile != NULL && *job->encoder_profile != '\0')
    {
        if (job->vcodec == HB_VCODEC_VT_H264)
        {
            if (!strcasecmp(job->encoder_profile, "baseline"))
            {
                pv->settings.profile = HB_VT_H264_PROFILE_BASELINE;
            }
            else if (!strcasecmp(job->encoder_profile, "main") ||
                    !strcasecmp(job->encoder_profile, "auto"))
            {
                pv->settings.profile = HB_VT_H264_PROFILE_MAIN;
            }
            else if (!strcasecmp(job->encoder_profile, "high"))
            {
                pv->settings.profile = HB_VT_H264_PROFILE_HIGH;
            }
            else
            {
                hb_error("encvt_Init: invalid profile '%s'", job->encoder_profile);
                return -1;
            }
        }
        else if (job->vcodec == HB_VCODEC_VT_H265)
        {
            if (!strcasecmp(job->encoder_profile, "main") ||
                !strcasecmp(job->encoder_profile, "auto"))
            {
                pv->settings.profile = HB_VT_H265_PROFILE_MAIN;
            }
            else
            {
                hb_error("encvt_Init: invalid profile '%s'", job->encoder_profile);
                return -1;
            }
        }
        else if (job->vcodec == HB_VCODEC_VT_H265_10BIT)
        {
            if (!strcasecmp(job->encoder_profile, "main10") ||
                !strcasecmp(job->encoder_profile, "auto"))
            {
                pv->settings.profile = HB_VT_H265_PROFILE_MAIN_10;
            }
            else if (!strcasecmp(job->encoder_profile, "main422-10"))
            {
                pv->settings.profile = HB_VT_H265_PROFILE_MAIN_422_10;
            }
            else
            {
                hb_error("encvt_Init: invalid profile '%s'", job->encoder_profile);
                return -1;
            }
        }
    }
    else
    {
        if (job->vcodec == HB_VCODEC_VT_H264)
        {
            pv->settings.profile = HB_VT_H264_PROFILE_HIGH;
        }
        else if (job->vcodec == HB_VCODEC_VT_H265)
        {
            pv->settings.profile = HB_VT_H265_PROFILE_MAIN;
        }
        else if (job->vcodec == HB_VCODEC_VT_H265_10BIT)
        {
            pv->settings.profile = HB_VT_H265_PROFILE_MAIN_10;
        }
    }

    pv->settings.encoderPixFmt = hb_vt_encoder_pixel_format_xlat(job->vcodec, pv->settings.profile, job->color_range);

    if (job->encoder_level != NULL && *job->encoder_level != '\0' && job->vcodec == HB_VCODEC_VT_H264)
    {
        int i;
        for (i = 0; hb_vt_h264_levels[i].name != NULL; i++)
        {
            if (!strcasecmp(job->encoder_level, hb_vt_h264_levels[i].name))
            {
                pv->settings.profileLevel = hb_vt_h264_levels[i].level[pv->settings.profile];
                break;
            }
        }
        if (hb_vt_h264_levels[i].name == NULL)
        {
            hb_error("encvt_Init: invalid level '%s'", job->encoder_level);
            *job->die = 1;
            return -1;
        }
    }
    else
    {
        if (job->vcodec == HB_VCODEC_VT_H264)
        {
            pv->settings.profileLevel = hb_vt_h264_levels[0].level[pv->settings.profile];
        }
        else if (job->vcodec == HB_VCODEC_VT_H265 || job->vcodec == HB_VCODEC_VT_H265_10BIT)
        {
            pv->settings.profileLevel = hb_vt_h265_levels[0].level[pv->settings.profile];
        }
    }

    // Compute the frame rate and output bit rate
    pv->settings.expectedFrameRate = (double)job->vrate.num / (double)job->vrate.den;

    if (job->vquality > HB_INVALID_VIDEO_QUALITY)
    {
        pv->settings.quality = job->vquality / 100;
        hb_log("encvt_Init: encoding with constant quality %f",
               pv->settings.quality * 100);
    }
    else if (job->vbitrate > 0)
    {
        pv->settings.averageBitRate = job->vbitrate * 1000;
        hb_log("encvt_Init: encoding with output bitrate %d Kbps",
               pv->settings.averageBitRate / 1000);
    }
    else
    {
        hb_error("encvt_Init: invalid rate control (bitrate %d, quality %f)",
                 job->vbitrate, job->vquality);
    }

    int fps                          = pv->settings.expectedFrameRate + 0.5;
    pv->settings.width               = job->width;
    pv->settings.height              = job->height;
    pv->settings.par.num             = job->par.num;
    pv->settings.par.den             = job->par.den;
    pv->settings.maxKeyFrameInterval = fps * 10 + 1;

    pv->settings.color.prim     = hb_output_color_prim(job);
    pv->settings.color.transfer = hb_output_color_transfer(job);
    pv->settings.color.matrix   = hb_output_color_matrix(job);
    pv->settings.color.chromaLocation = job->chroma_location;

    // HDR10 Static metadata
    if (job->color_transfer == HB_COLR_TRA_SMPTEST2084)
    {
        // Mastering display metadata
        if (job->mastering.has_primaries && job->mastering.has_luminance)
        {
            pv->settings.color.masteringDisplay = hb_vt_mastering_display_xlat(job->mastering);
        }

        //  Content light level
        if (job->coll.max_cll && job->coll.max_fall)
        {
            pv->settings.color.contentLightLevel = hb_vt_content_light_level_xlat(job->coll);
        }
    }

    if (job->ambient.ambient_illuminance.num && job->ambient.ambient_illuminance.den)
    {
        pv->settings.color.ambientViewingEnviroment = hb_vt_ambient_viewing_enviroment_xlat(job->ambient);
    }

    return 0;
}

static int hb_vt_parse_options(hb_work_private_t *pv, hb_job_t *job)
{
    hb_dict_t *opts = NULL;
    if (job->encoder_options != NULL && *job->encoder_options)
    {
        opts = hb_encopts_to_dict(job->encoder_options, job->vcodec);
    }

    hb_dict_iter_t iter;
    for (iter  = hb_dict_iter_init(opts);
         iter != HB_DICT_ITER_DONE;
         iter  = hb_dict_iter_next(opts, iter))
    {
        const char *key = hb_dict_iter_key(iter);
        hb_value_t *value = hb_dict_iter_value(iter);

        if (!strcmp(key, "keyint"))
        {
            int keyint = hb_value_get_int(value);
            if (keyint >= 0)
            {
                pv->settings.maxKeyFrameInterval = keyint;
            }
        }
        else if (!strcmp(key, "bframes"))
        {
            int enabled = hb_value_get_bool(value);
            pv->settings.allowFrameReordering = enabled ? kCFBooleanTrue : kCFBooleanFalse;
        }
        else if (!strcmp(key, "cabac"))
        {
            int enabled = hb_value_get_bool(value);
            pv->settings.h264.entropyMode = enabled ? kVTH264EntropyMode_CABAC : kVTH264EntropyMode_CAVLC;
        }
        else if (!strcmp(key, "vbv-bufsize"))
        {
            int bufsize = hb_value_get_int(value);
            if (bufsize > 0)
            {
                pv->settings.vbv.bufsize = bufsize;
            }
        }
        else if (!strcmp(key, "vbv-maxrate"))
        {
            int maxrate = hb_value_get_int(value);
            if (maxrate > 0)
            {
                pv->settings.vbv.maxrate = maxrate;
            }
        }
        else if (!strcmp(key, "slice-max-size"))
        {
            int slice_max_size = hb_value_get_bool(value);
            if (slice_max_size > 0)
            {
                pv->settings.h264.maxSliceBytes = slice_max_size;
            }
        }
        else if (!strcmp(key, "max-frame-delay"))
        {
            int maxdelay = hb_value_get_bool(value);
            if (maxdelay > 0)
            {
                pv->settings.maxFrameDelayCount = maxdelay;
            }
        }
        else if (!strcmp(key, "gpu-registryid"))
        {
            uint64_t registryID = hb_value_get_int(value);
            if (registryID > 0)
            {
                pv->settings.registryID = registryID;
            }
        }
        else if (!strcmp(key, "qpmin"))
        {
            int qpmin = hb_value_get_int(value);
            if (qpmin >= 0)
            {
                pv->settings.minAllowedFrameQP = qpmin;
            }
        }
        else if (!strcmp(key, "qpmax"))
        {
            int qpmax = hb_value_get_int(value);
            if (qpmax >= 0)
            {
                pv->settings.maxAllowedFrameQP = qpmax;
            }
        }
        else if (!strcmp(key, "ref"))
        {
            int ref = hb_value_get_int(value);
            if (ref >= 0)
            {
                pv->settings.maxReferenceBufferCount = ref;
            }
        }
        else if (!strcmp(key, "look-ahead-frame-count"))
        {
            int lookaheadframe = hb_value_get_int(value);
            if (lookaheadframe >= 0)
            {
                pv->settings.lookAheadFrameCount = lookaheadframe;
            }
        }
        else if (!strcmp(key, "disable-spatial-adaptive-qp"))
        {
            int disabled = hb_value_get_bool(value);
            pv->settings.disableSpatialAdaptiveQP = disabled ? kCFBooleanTrue : kCFBooleanFalse;
        }
        else
        {
            hb_log("encvt_Init: unknown option '%s'", key);
        }
    }
    hb_dict_free(&opts);

    // Sanitize interframe settings
    switch (pv->settings.maxKeyFrameInterval)
    {
        case 1:
            pv->settings.allowTemporalCompression = kCFBooleanFalse;
        case 2:
            pv->settings.allowFrameReordering     = kCFBooleanFalse;
        default:
            break;
    }
    switch (pv->settings.maxFrameDelayCount)
    {
        case 0:
            pv->settings.allowTemporalCompression = kCFBooleanFalse;
        case 1:
            pv->settings.allowFrameReordering     = kCFBooleanFalse;
        default:
            break;
    }

    if (pv->settings.lookAheadFrameCount > pv->settings.maxFrameDelayCount &&
        pv->settings.maxFrameDelayCount != kVTUnlimitedFrameDelayCount)
    {
        pv->settings.lookAheadFrameCount = pv->settings.maxFrameDelayCount;
    }

    if (pv->settings.quality == 1 || pv->passStorage)
    {
        pv->settings.lookAheadFrameCount = -1;
    }

    return 0;
}

static void hb_vt_set_data_rate_limits(VTCompressionSessionRef session, int bufsize, int maxrate)
{
    float seconds = ((float)bufsize / (float)maxrate);
    int bytes = maxrate * 125 * seconds;

    CFNumberRef size = CFNumberCreate(kCFAllocatorDefault,
                                      kCFNumberIntType, &bytes);
    CFNumberRef duration = CFNumberCreate(kCFAllocatorDefault,
                                          kCFNumberFloatType, &seconds);
    CFMutableArrayRef dataRateLimits = CFArrayCreateMutable(kCFAllocatorDefault, 2,
                                                            &kCFTypeArrayCallBacks);
    CFArrayAppendValue(dataRateLimits, size);
    CFArrayAppendValue(dataRateLimits, duration);

    hb_vt_set_property(session, kVTCompressionPropertyKey_DataRateLimits, dataRateLimits);

    CFRelease(size);
    CFRelease(duration);
    CFRelease(dataRateLimits);
}

static CVPixelBufferRef hb_vt_get_pix_buf(hb_work_private_t *pv, hb_buffer_t *buf)
{
    CVPixelBufferRef pix_buf = NULL;

    if (pv->job->hw_pix_fmt == AV_PIX_FMT_VIDEOTOOLBOX)
    {
        pix_buf = hb_cv_get_pixel_buffer(buf);
        if (pix_buf)
        {
            CVPixelBufferRetain(pix_buf);
        }
    }
    else
    {
        int numberOfPlanes = pv->settings.inputPixFmt == kCVPixelFormatType_420YpCbCr8Planar ||
        pv->settings.inputPixFmt == kCVPixelFormatType_420YpCbCr8PlanarFullRange ? 3 : 2;

        void *planeBaseAddress[3]  = {buf->plane[0].data,   buf->plane[1].data,   buf->plane[2].data};
        size_t planeWidth[3]       = {buf->plane[0].width,  buf->plane[1].width,  buf->plane[2].width};
        size_t planeHeight[3]      = {buf->plane[0].height, buf->plane[1].height, buf->plane[2].height};
        size_t planeBytesPerRow[3] = {buf->plane[0].stride, buf->plane[1].stride, buf->plane[2].stride};

        OSStatus err = CVPixelBufferCreateWithPlanarBytes(
                                                 kCFAllocatorDefault,
                                                 buf->f.width,
                                                 buf->f.height,
                                                 pv->settings.inputPixFmt,
                                                 buf->data,
                                                 0,
                                                 numberOfPlanes,
                                                 planeBaseAddress,
                                                 planeWidth,
                                                 planeHeight,
                                                 planeBytesPerRow,
                                                 NULL,
                                                 buf,
                                                 NULL,
                                                 &pix_buf);
        if (err)
        {
            pix_buf = NULL;
        }
    }

    return pix_buf;
}

static void hb_vt_insert_dynamic_metadata(hb_work_private_t *pv, CMSampleBufferRef sampleBuffer, hb_buffer_t **buf)
{
    if (pv->job->passthru_dynamic_hdr_metadata == 0)
    {
        return;
    }

    if (pv->nal_length_size == 0)
    {
        pv->nal_length_size = hb_vt_get_nal_length_size(sampleBuffer, pv->settings.codec);
    }

    if (pv->nal_length_size > 4)
    {
        hb_log("VTCompressionSession: unknown nal length size");
        return;
    }

    hb_buffer_t *buf_in = *buf;
    hb_sei_t seis[4];
    size_t seis_count = 0;

    if (buf_in->s.frametype == HB_FRAME_IDR)
    {
        if (pv->settings.color.contentLightLevel)
        {
            const uint8_t *coll_data = CFDataGetBytePtr(pv->settings.color.contentLightLevel);
            size_t coll_size = CFDataGetLength(pv->settings.color.contentLightLevel);

            seis[seis_count].type = HB_CONTENT_LIGHT_LEVEL_INFO;
            seis[seis_count].payload = coll_data;
            seis[seis_count].payload_size = coll_size;

            seis_count += 1;
        }

        if (pv->settings.color.masteringDisplay)
        {
            const uint8_t *mastering_data = CFDataGetBytePtr(pv->settings.color.masteringDisplay);
            size_t mastering_size = CFDataGetLength(pv->settings.color.masteringDisplay);

            seis[seis_count].type = HB_MASTERING_DISPLAY_INFO;
            seis[seis_count].payload = mastering_data;
            seis[seis_count].payload_size = mastering_size;

            seis_count += 1;
        }
    }

    if (pv->job->passthru_dynamic_hdr_metadata & HB_HDR_DYNAMIC_METADATA_HDR10PLUS)
    {
        CFDataRef hdrPlus = CMGetAttachment(sampleBuffer, CFSTR("HB_HDR_PLUS"), NULL);
        if (hdrPlus != NULL)
        {
            const uint8_t *sei_data = CFDataGetBytePtr(hdrPlus);
            size_t sei_size = CFDataGetLength(hdrPlus);

            seis[seis_count].type = HB_USER_DATA_REGISTERED_ITU_T_T35;
            seis[seis_count].payload = sei_data;
            seis[seis_count].payload_size = sei_size;

            seis_count += 1;
        }
    }

    hb_nal_t nals[1];
    size_t nals_count = 0;

    if (pv->job->passthru_dynamic_hdr_metadata & HB_HDR_DYNAMIC_METADATA_DOVI)
    {
        CFDataRef rpu = CMGetAttachment(sampleBuffer, CFSTR("HB_DOVI_RPU"), NULL);
        if (rpu != NULL)
        {
            const uint8_t *rpu_data = CFDataGetBytePtr(rpu);
            size_t rpu_size = CFDataGetLength(rpu);

            nals[nals_count].type = HB_HEVC_NAL_UNIT_UNSPECIFIED;
            nals[nals_count].payload = rpu_data;
            nals[nals_count].payload_size = rpu_size;

            nals_count += 1;
        }
    }

    if (seis_count || nals_count)
    {
        hb_buffer_t *out = hb_isomp4_hevc_nal_bitstream_insert_payloads(buf_in->data, buf_in->size,
                                                                        seis, seis_count,
                                                                        nals, nals_count,
                                                                        pv->nal_length_size);
        if (out)
        {
            out->f = buf_in->f;
            out->s = buf_in->s;
            hb_buffer_close(buf);
            *buf = out;
        }
    }
}

static void hb_vt_set_frametype(CMSampleBufferRef sampleBuffer, hb_buffer_t *buf)
{
    buf->s.frametype = HB_FRAME_IDR;
    buf->s.flags |= HB_FLAG_FRAMETYPE_REF;

    CFArrayRef attachmentsArray = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, 0);
    if (CFArrayGetCount(attachmentsArray))
    {
        CFDictionaryRef dict = CFArrayGetValueAtIndex(attachmentsArray, 0);
        CFBooleanRef notSync;
        if (CFDictionaryGetValueIfPresent(dict, kCMSampleAttachmentKey_NotSync,(const void **) &notSync))
        {
            Boolean notSyncValue = CFBooleanGetValue(notSync);
            if (notSyncValue)
            {
                CFBooleanRef b;
                if (CFDictionaryGetValueIfPresent(dict, kCMSampleAttachmentKey_PartialSync, NULL))
                {
                    buf->s.frametype = HB_FRAME_I;
                }
                else if (CFDictionaryGetValueIfPresent(dict, kCMSampleAttachmentKey_IsDependedOnByOthers,(const void **) &b))
                {
                    Boolean bv = CFBooleanGetValue(b);
                    if (bv)
                    {
                        buf->s.frametype = HB_FRAME_P;
                    }
                    else
                    {
                        buf->s.frametype = HB_FRAME_B;
                        buf->s.flags &= ~HB_FLAG_FRAMETYPE_REF;
                    }
                }
                else
                {
                   buf->s.frametype = HB_FRAME_P;
                }
            }
        }
    }
}

static void hb_vt_set_timestamps(hb_work_private_t *pv, CMSampleBufferRef sampleBuffer, hb_buffer_t *buf)
{
    CMTime presentationTimeStamp = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    CMTime duration = CMSampleBufferGetDuration(sampleBuffer);

    buf->s.duration = duration.value;
    buf->s.start = presentationTimeStamp.value;
    buf->s.stop  = presentationTimeStamp.value + buf->s.duration;

    // Use the cached frame info to get the start time of Nth frame
    // Note that start Nth frame != start time this buffer since the
    // output buffers have rearranged start times.
    if (pv->frameno_out < pv->job->areBframes)
    {
        buf->s.renderOffset = hb_vt_get_frame_start(pv, pv->frameno_out) - pv->dts_delay;
    }
    else
    {
        buf->s.renderOffset = hb_vt_get_frame_start(pv, pv->frameno_out - pv->job->areBframes);
    }
    pv->frameno_out++;
}

static hb_buffer_t * hb_vt_get_buf(CMSampleBufferRef sampleBuffer, hb_work_private_t *pv)
{
    CMItemCount samplesNum = CMSampleBufferGetNumSamples(sampleBuffer);
    if (samplesNum > 1)
    {
        hb_log("VTCompressionSession: more than 1 sample in sampleBuffer = %ld", samplesNum);
    }

    hb_buffer_t *buf = NULL;
    CMBlockBufferRef buffer = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (buffer)
    {
        size_t sampleSize = CMBlockBufferGetDataLength(buffer);
        Boolean isContiguous = CMBlockBufferIsRangeContiguous(buffer, 0, sampleSize);

        if (isContiguous)
        {
            size_t lengthAtOffsetOut, totalLengthOut;
            char * _Nullable dataPointerOut;

            buf = hb_buffer_wrapper_init();
            OSStatus err = CMBlockBufferGetDataPointer(buffer, 0, &lengthAtOffsetOut, &totalLengthOut, &dataPointerOut);
            if (err != kCMBlockBufferNoErr)
            {
                hb_log("VTCompressionSession: CMBlockBufferGetDataPointer error");
            }
            buf->data = (uint8_t *)dataPointerOut;
            buf->size = totalLengthOut;
            buf->storage = sampleBuffer;
            buf->storage_type = COREMEDIA;
            CFRetain(sampleBuffer);
        }
        else
        {
            buf = hb_buffer_init(sampleSize);
            OSStatus err = CMBlockBufferCopyDataBytes(buffer, 0, sampleSize, buf->data);
            if (err != kCMBlockBufferNoErr)
            {
                hb_log("VTCompressionSession: CMBlockBufferCopyDataBytes error");
            }
        }

        hb_vt_set_frametype(sampleBuffer, buf);
        hb_vt_set_timestamps(pv, sampleBuffer, buf);
        hb_vt_insert_dynamic_metadata(pv, sampleBuffer, &buf);

        if (buf->s.frametype == HB_FRAME_IDR)
        {
            hb_chapter_dequeue(pv->chapter_queue, buf);
        }
    }

    return buf;
}

void hb_vt_compression_output_callback(
                                   void *outputCallbackRefCon,
                                   void *sourceFrameRefCon,
                                   OSStatus status,
                                   VTEncodeInfoFlags infoFlags,
                                   CMSampleBufferRef sampleBuffer)
{
    OSStatus err;

    if (sourceFrameRefCon)
    {
        hb_buffer_t *buf = (hb_buffer_t *)sourceFrameRefCon;
        if (sampleBuffer)
        {
            hb_vt_add_dynamic_hdr_metadata(sampleBuffer, buf);
        }
        hb_buffer_close(&buf);
    }

    if (status != noErr)
    {
        hb_log("VTCompressionSession: hb_vt_compression_output_callback called error");
    }
    else if (sampleBuffer)
    {
        CFRetain(sampleBuffer);
        CMSimpleQueueRef queue = outputCallbackRefCon;
        err = CMSimpleQueueEnqueue(queue, sampleBuffer);
        if (err)
        {
            hb_log("VTCompressionSession: hb_vt_compression_output_callback queue full");
        }
    }
    else
    {
        hb_log("VTCompressionSession: hb_vt_compression_output_callback sample buffer is NULL");
    }
}

static OSStatus hb_vt_init_session(hb_work_object_t *w, hb_job_t *job, hb_work_private_t *pv, int cookieOnly)
{
    OSStatus err = noErr;
    CFNumberRef cfValue = NULL;

    CFMutableDictionaryRef encoderSpecifications = CFDictionaryCreateMutable(
                                                                             kCFAllocatorDefault,
                                                                             2,
                                                                             &kCFTypeDictionaryKeyCallBacks,
                                                                             &kCFTypeDictionaryValueCallBacks);

    CFDictionaryAddValue(encoderSpecifications, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder, kCFBooleanTrue);

    if (pv->settings.registryID > 0)
    {
        cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberLongLongType,
                                 &pv->settings.registryID);
        if (__builtin_available(macOS 10.14, *))
        {
            CFDictionaryAddValue(encoderSpecifications, kVTVideoEncoderSpecification_RequiredEncoderGPURegistryID, cfValue);
        }
        CFRelease(cfValue);
    }

    OSType cv_pix_fmt = pv->settings.encoderPixFmt;
    CFNumberRef pix_fmt_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &cv_pix_fmt);

    const void *attrs_keys[1] = { kCVPixelBufferPixelFormatTypeKey };
    const void *attrs_values[1] = { pix_fmt_num };

    CFDictionaryRef imageBufferAttributes = CFDictionaryCreate(kCFAllocatorDefault,
                                                               attrs_keys, attrs_values, 1,
                                                               &kCFTypeDictionaryKeyCallBacks,
                                                               &kCFTypeDictionaryValueCallBacks);
    CFRelease(pix_fmt_num);

    CMSimpleQueueCreate(kCFAllocatorDefault, 200, &pv->queue);

    err = VTCompressionSessionCreate(
                               kCFAllocatorDefault,
                               pv->settings.width,
                               pv->settings.height,
                               pv->settings.codec,
                               encoderSpecifications,
                               imageBufferAttributes,
                               NULL,
                               &hb_vt_compression_output_callback,
                               pv->queue,
                               &pv->session);

    CFRelease(imageBufferAttributes);

    if (err != noErr)
    {
        hb_log("Error creating a VTCompressionSession err=%"PRId64"", (int64_t)err);
        CFRelease(encoderSpecifications);
        return err;
    }

    // Print the actual encoderID
    if (cookieOnly == 0)
    {
        CFStringRef encoderID;
        err = VTSessionCopyProperty(pv->session,
                                    kVTCompressionPropertyKey_EncoderID,
                                    kCFAllocatorDefault,
                                    &encoderID);

        if (err == noErr)
        {
            static const int VAL_BUF_LEN = 256;
            char valBuf[VAL_BUF_LEN];

            Boolean haveStr = CFStringGetCString(encoderID,
                                                 valBuf,
                                                 VAL_BUF_LEN,
                                                 kCFStringEncodingUTF8);
            if (haveStr)
            {
                hb_log("encvt_Init: %s", valBuf);
            }
            CFRelease(encoderID);
        }
    }

    CFDictionaryRef supportedProps = NULL;
    err = VTCopySupportedPropertyDictionaryForEncoder(pv->settings.width,
                                                      pv->settings.height,
                                                      pv->settings.codec,
                                                      encoderSpecifications,
                                                      NULL,
                                                      &supportedProps);

    if (err != noErr)
    {
        hb_log("Error retrieving the supported property dictionary err=%"PRId64"", (int64_t)err);
    }

    CFRelease(encoderSpecifications);

    // Offline encoders (such as Handbrake) should set RealTime property to False, as it disconnects the relationship
    // between encoder speed and target video frame rate, explicitly setting RealTime to false encourages VideoToolbox
    // to use the fastest mode, while adhering to the required output quality/bitrate and favorQualityOverSpeed settings
    hb_vt_set_property(pv->session,
                       kVTCompressionPropertyKey_RealTime,
                       kCFBooleanFalse);

    hb_vt_set_property(pv->session,
                       kVTCompressionPropertyKey_AllowTemporalCompression,
                       pv->settings.allowTemporalCompression);

    hb_vt_set_property(pv->session,
                       kVTCompressionPropertyKey_AllowFrameReordering,
                       pv->settings.allowFrameReordering);

    cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                             &pv->settings.maxKeyFrameInterval);
    hb_vt_set_property(pv->session,
                       kVTCompressionPropertyKey_MaxKeyFrameInterval,
                       cfValue);
    CFRelease(cfValue);

    if (__builtin_available(macOS 11, *))
    {
        if (supportedProps != NULL && CFDictionaryContainsKey(supportedProps, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality))
        {
            hb_vt_set_property(pv->session,
                               kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality,
                               pv->settings.prioritizeEncodingSpeedOverQuality);
        }
    }

    if (__builtin_available(macOS 12, *))
    {
        if (pv->settings.maxAllowedFrameQP > -1)
        {
            cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                     &pv->settings.maxAllowedFrameQP);
            hb_vt_set_property(pv->session,
                               kVTCompressionPropertyKey_MaxAllowedFrameQP,
                               cfValue);
            CFRelease(cfValue);
        }
    }

    if (__builtin_available(macOS 13, *))
    {
        if (pv->settings.minAllowedFrameQP > -1)
        {
            cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                     &pv->settings.minAllowedFrameQP);
            hb_vt_set_property(pv->session,
                               kVTCompressionPropertyKey_MinAllowedFrameQP,
                               cfValue);
            CFRelease(cfValue);
        }

        if (pv->settings.maxReferenceBufferCount > -1)
        {
            cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                     &pv->settings.maxReferenceBufferCount);
            hb_vt_set_property(pv->session,
                               kVTCompressionPropertyKey_ReferenceBufferCount,
                               cfValue);
            CFRelease(cfValue);
        }
    }

    if (pv->settings.maxFrameDelayCount >= 0)
    {
        cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                 &pv->settings.maxFrameDelayCount);
        hb_vt_set_property(pv->session,
                           kVTCompressionPropertyKey_MaxFrameDelayCount,
                           cfValue);
        CFRelease(cfValue);
    }

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
    if (__builtin_available(macOS 15, *))
    {
        // Control spatial adaptation of the quantization parameter (QP) based on per-frame statistics.
        if (pv->settings.disableSpatialAdaptiveQP == kCFBooleanTrue)
        {
            int32_t spatialAdaptiveQP = kVTQPModulationLevel_Disable;
            cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
                                     &spatialAdaptiveQP);
            hb_vt_set_property(pv->session,
                               kVTCompressionPropertyKey_SpatialAdaptiveQPLevel,
                               cfValue);
            CFRelease(cfValue);
        }

        // Requests that the encoder retain the specified number of frames during encoding.
        if (pv->settings.lookAheadFrameCount >= 0)
        {
            cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                     &pv->settings.lookAheadFrameCount);
            hb_vt_set_property(pv->session,
                               kVTCompressionPropertyKey_SuggestedLookAheadFrameCount,
                               cfValue);
            CFRelease(cfValue);
        }
    }
#endif

    if (
#if defined(__aarch64__)
        job->pass_id == HB_PASS_ENCODE &&
#endif
        pv->settings.vbv.maxrate > 0 &&
        pv->settings.vbv.bufsize > 0)
    {
        hb_vt_set_data_rate_limits(pv->session, pv->settings.vbv.bufsize, pv->settings.vbv.maxrate);
    }

    if (pv->settings.fieldDetail != HB_VT_FIELDORDER_PROGRESSIVE)
    {
        int count = 2;
        cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &count);
        hb_vt_set_property(pv->session,
                           kVTCompressionPropertyKey_FieldCount,
                           cfValue);
        CFRelease(cfValue);

        CFStringRef cfStringValue = NULL;
        switch (pv->settings.fieldDetail)
        {
            case HB_VT_FIELDORDER_BFF:
                cfStringValue = kCMFormatDescriptionFieldDetail_TemporalBottomFirst;
                break;
            case HB_VT_FIELDORDER_TFF:
            default:
                cfStringValue = kCMFormatDescriptionFieldDetail_TemporalTopFirst;
                break;
        }
        hb_vt_set_property(pv->session,
                           kVTCompressionPropertyKey_FieldDetail,
                           cfStringValue);
    }

    hb_vt_set_property(pv->session,
                       kVTCompressionPropertyKey_ColorPrimaries,
                       hb_cv_colr_pri_xlat(pv->settings.color.prim));
    hb_vt_set_property(pv->session,
                       kVTCompressionPropertyKey_TransferFunction,
                       hb_cv_colr_tra_xlat(pv->settings.color.transfer));
    CFNumberRef gamma = hb_cv_colr_gamma_xlat(pv->settings.color.transfer);
    if (gamma)
    {
        hb_vt_set_property(pv->session,
                           CFSTR("GammaLevel"),
                           gamma);
        CFRelease(gamma);
    }
    hb_vt_set_property(pv->session,
                       kVTCompressionPropertyKey_YCbCrMatrix,
                       hb_cv_colr_mat_xlat(pv->settings.color.matrix));
    hb_vt_set_property(pv->session,
                       CFSTR("ChromaLocationTopField"),
                       hb_cv_chroma_loc_xlat(pv->settings.color.chromaLocation));
    hb_vt_set_property(pv->session,
                       CFSTR("ChromaLocationBottomField"),
                       hb_cv_chroma_loc_xlat(pv->settings.color.chromaLocation));

    if (supportedProps != NULL && CFDictionaryContainsKey(supportedProps, kVTCompressionPropertyKey_MasteringDisplayColorVolume) &&
        pv->settings.color.masteringDisplay != NULL)
    {
        hb_vt_set_property(pv->session,
                           kVTCompressionPropertyKey_MasteringDisplayColorVolume,
                           pv->settings.color.masteringDisplay);
    }

    if (supportedProps != NULL && CFDictionaryContainsKey(supportedProps, kVTCompressionPropertyKey_ContentLightLevelInfo) &&
        pv->settings.color.contentLightLevel != NULL)
    {
        hb_vt_set_property(pv->session,
                           kVTCompressionPropertyKey_ContentLightLevelInfo,
                           pv->settings.color.contentLightLevel);
    }

    if (supportedProps != NULL && CFDictionaryContainsKey(supportedProps, CFSTR("AmbientViewingEnvironment")) &&
        pv->settings.color.ambientViewingEnviroment != NULL)
    {
        hb_vt_set_property(pv->session,
                           CFSTR("AmbientViewingEnvironment"),
                           pv->settings.color.ambientViewingEnviroment);
    }

    if (__builtin_available(macOS 11.0, *))
    {
        if (pv->settings.codec != kCMVideoCodecType_H264)
        {
            // VideoToolbox can generate Dolby Vision 8.4 RPUs for HLG video,
            // however we preserve the RPUs from the source file, so disable it
            // to avoid having two sets of RPUs per frame.
            if (supportedProps != NULL && CFDictionaryContainsKey(supportedProps, kVTCompressionPropertyKey_HDRMetadataInsertionMode))
            {
                hb_vt_set_property(pv->session,
                                   kVTCompressionPropertyKey_HDRMetadataInsertionMode,
                                   kVTHDRMetadataInsertionMode_None);
            }

            if (supportedProps != NULL && CFDictionaryContainsKey(supportedProps, kVTCompressionPropertyKey_PreserveDynamicHDRMetadata))
            {
                hb_vt_set_property(pv->session,
                                   kVTCompressionPropertyKey_PreserveDynamicHDRMetadata,
                                   pv->settings.preserveDynamicHDRMetadata);
            }
        }
    }

    CFNumberRef parNum = CFNumberCreate(kCFAllocatorDefault,
                                        kCFNumberSInt32Type,
                                        &pv->settings.par.num);
    CFNumberRef parDen = CFNumberCreate(kCFAllocatorDefault,
                                        kCFNumberSInt32Type,
                                        &pv->settings.par.den);
    CFMutableDictionaryRef pixelAspectRatio = CFDictionaryCreateMutable(kCFAllocatorDefault, 2,
                                                                        &kCFTypeDictionaryKeyCallBacks,
                                                                        &kCFTypeDictionaryValueCallBacks);
    CFDictionaryAddValue(pixelAspectRatio,
                         kCMFormatDescriptionKey_PixelAspectRatioHorizontalSpacing,
                         parNum);
    CFDictionaryAddValue(pixelAspectRatio,
                         kCMFormatDescriptionKey_PixelAspectRatioVerticalSpacing,
                         parDen);

    hb_vt_set_property(pv->session,
                       kVTCompressionPropertyKey_PixelAspectRatio,
                       pixelAspectRatio);
    CFRelease(parNum);
    CFRelease(parDen);
    CFRelease(pixelAspectRatio);

    cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType,
                             &pv->settings.expectedFrameRate);
    hb_vt_set_property(pv->session,
                       kVTCompressionPropertyKey_ExpectedFrameRate,
                       cfValue);
    CFRelease(cfValue);

    if (pv->settings.quality > -1)
    {
        cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType,
                                 &pv->settings.quality);
        hb_vt_set_property(pv->session,
                           kVTCompressionPropertyKey_Quality,
                           cfValue);
        CFRelease(cfValue);
    }
    else
    {
        cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                 &pv->settings.averageBitRate);
        hb_vt_set_property(pv->session,
                           kVTCompressionPropertyKey_AverageBitRate,
                           cfValue);
        CFRelease(cfValue);
    }

    hb_vt_set_property(pv->session,
                       kVTCompressionPropertyKey_ProfileLevel,
                       pv->settings.profileLevel);

    if (pv->settings.codec == kCMVideoCodecType_H264)
    {
        if (pv->settings.h264.entropyMode)
        {
            hb_vt_set_property(pv->session,
                               kVTCompressionPropertyKey_H264EntropyMode,
                               pv->settings.h264.entropyMode);
        }
        if (pv->settings.h264.maxSliceBytes)
        {
            cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                     &pv->settings.h264.maxSliceBytes);
            hb_vt_set_property(pv->session,
                               kVTCompressionPropertyKey_MaxH264SliceBytes,
                               cfValue);
            CFRelease(cfValue);
        }
    }

    if (supportedProps)
    {
        CFRelease(supportedProps);
    }

    // Multi-pass
    if (job->pass_id == HB_PASS_ENCODE_ANALYSIS)
    {
        char *filename = hb_get_temporary_filename("videotoolbox.log");;

        CFURLRef url = NULL;
        if (filename)
        {
            url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)filename,
                                                          strlen(filename), false);
            free(filename);
        }

        if (url == NULL)
        {
            return -1;
        }

        err = VTMultiPassStorageCreate(kCFAllocatorDefault, url, kCMTimeRangeInvalid, NULL, &pv->passStorage);
        CFRelease(url);

        if (err != noErr)
        {
            return err;
        }

        hb_vt_set_property(pv->session,
                           kVTCompressionPropertyKey_MultiPassStorage,
                           pv->passStorage);

        err = VTCompressionSessionBeginPass(pv->session, 0, 0);
        if (err != noErr)
        {
            hb_log("VTCompressionSessionBeginPass failed");
        }
    }

    err = VTCompressionSessionPrepareToEncodeFrames(pv->session);
    if (err != noErr)
    {
        hb_log("VTCompressionSessionPrepareToEncodeFrames failed");
        return err;
    }

    CFBooleanRef allowFrameReordering;
    err = VTSessionCopyProperty(pv->session,
                                kVTCompressionPropertyKey_AllowFrameReordering,
                                kCFAllocatorDefault,
                                &allowFrameReordering);
    if (err != noErr)
    {
        hb_log("VTSessionCopyProperty: kVTCompressionPropertyKey_AllowFrameReordering failed");
    }
    else
    {
        if (CFBooleanGetValue(allowFrameReordering))
        {
            // There is no way to know if b-pyramid will be
            // used or not, to be safe always assume it's enabled
            job->areBframes = 2;
        }
        CFRelease(allowFrameReordering);
    }

    return err;
}

static void hb_vt_set_cookie(hb_work_object_t *w, CMFormatDescriptionRef format)
{
    CFDictionaryRef extentions = CMFormatDescriptionGetExtensions(format);
    if (!extentions)
    {
        hb_log("VTCompressionSession: Format Description Extensions error");
    }
    else
    {
        CFStringRef key = CMVideoFormatDescriptionGetCodecType(format) == kCMVideoCodecType_H264 ? CFSTR("avcC") : CFSTR("hvcC");
        CFDictionaryRef atoms = CFDictionaryGetValue(extentions, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
        if (atoms)
        {
            CFDataRef magicCookie = CFDictionaryGetValue(atoms, key);

            if (magicCookie)
            {
                const uint8_t *hvcCAtom = CFDataGetBytePtr(magicCookie);
                CFIndex size = CFDataGetLength(magicCookie);
                hb_set_extradata(w->extradata, hvcCAtom, size);
            }
            else
            {
                hb_log("VTCompressionSession: Magic Cookie error");
            }
        }
    }
}

static OSStatus hb_vt_create_cookie(hb_work_object_t *w, hb_job_t *job, hb_work_private_t *pv)
{
    OSStatus err;
    CVPixelBufferRef pix_buf = NULL;
    CVPixelBufferPoolRef pool = NULL;

    err = hb_vt_init_session(w, job, pv, 1);
    if (err != noErr)
    {
        goto fail;
    }

    pool = VTCompressionSessionGetPixelBufferPool(pv->session);

    if (pool == NULL)
    {
        hb_log("VTCompressionSession: VTCompressionSessionGetPixelBufferPool error");
        err = -1;
        goto fail;
    }

    err = CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pix_buf);

    if (kCVReturnSuccess != err)
    {
        hb_log("VTCompressionSession: CVPixelBufferPoolCreatePixelBuffer error");
    }

    CMTime pts = CMTimeMake(0, pv->settings.timescale);
    CMTime duration = CMTimeMake(pv->settings.timescale, pv->settings.timescale);
    err = VTCompressionSessionEncodeFrame(
                                          pv->session,
                                          pix_buf,
                                          pts,
                                          duration,
                                          NULL,
                                          NULL,
                                          NULL);
    if (noErr != err)
    {
        hb_log("VTCompressionSession: VTCompressionSessionEncodeFrame error");
    }
    err = VTCompressionSessionCompleteFrames(pv->session, kCMTimeIndefinite);
    if (noErr != err)
    {
        hb_log("VTCompressionSession: VTCompressionSessionCompleteFrames error");
    }
    CMSampleBufferRef sampleBuffer = (CMSampleBufferRef)CMSimpleQueueDequeue(pv->queue);

    if (!sampleBuffer)
    {
        hb_log("VTCompressionSession: sampleBuffer == NULL");
        goto fail;
    }
    else
    {
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (!format)
        {
            hb_log("VTCompressionSession: Format Description error");
        }
        else
        {
            pv->format = format;
            CFRetain(pv->format);
            hb_vt_set_cookie(w, format);
        }
        CFRelease(sampleBuffer);
    }

fail:
    CVPixelBufferRelease(pix_buf);
    VTCompressionSessionInvalidate(pv->session);
    if (pv->passStorage)
    {
        VTMultiPassStorageClose(pv->passStorage);
        CFRelease(pv->passStorage);
    }
    if (pv->session)
    {
        CFRelease(pv->session);
    }
    if (pv->queue)
    {
        CFRelease(pv->queue);
    }
    pv->session = NULL;
    pv->passStorage = NULL;
    pv->queue = NULL;

    return err;
}

static OSStatus hb_vt_reuse_session(hb_work_object_t *w, hb_job_t * job, hb_work_private_t *pv)
{
    OSStatus err = noErr;

    hb_interjob_t *interjob = hb_interjob_get(job->h);
    vt_interjob_t *context  = interjob->context;

    hb_vt_set_cookie(w, context->format);

    pv->session     = context->session;
    pv->passStorage = context->passStorage;
    pv->queue       = context->queue;
    pv->format      = context->format;
    job->areBframes = context->areBframes;

    if (err != noErr)
    {
        hb_log("Error reusing a VTCompressionSession err=%"PRId64"", (int64_t)err);
        return err;
    }

    // This should tell us the time range the encoder thinks it can enhance in the second pass
    // currently we ignore this, because it would mean storing the frames from the first pass
    // somewhere on disk.
    // And it seems the range is always the entire movie duration.
    err = VTCompressionSessionGetTimeRangesForNextPass(pv->session, &pv->timeRangeCount, &pv->timeRangeArray);

    if (err != noErr)
    {
        hb_log("Error beginning a VTCompressionSession final pass err=%"PRId64"", (int64_t)err);
        return err;
    }

    hb_log("encvt_Init: starting pass with time ranges: %ld", pv->timeRangeCount);

    for (CMItemCount i = 0; i < pv->timeRangeCount; i++)
    {
        hb_log("encvt_init: %lld, %lld",
               pv->timeRangeArray[i].start.value,
               pv->timeRangeArray[i].duration.value);
    }

    err = VTCompressionSessionBeginPass(pv->session, kVTCompressionSessionBeginFinalPass, 0);

    if (err != noErr)
    {
        hb_log("Error beginning a VTCompressionSession final pass err=%"PRId64"", (int64_t)err);
        return err;
    }

    free(context);
    interjob->context = NULL;

    return err;
}

int encvt_init(hb_work_object_t *w, hb_job_t *job)
{
    OSStatus err;
    hb_work_private_t *pv = calloc(1, sizeof(hb_work_private_t));
    if (pv == NULL)
    {
        *job->die = 1;
        return -1;
    }
    w->private_data = pv;

    pv->job = job;
    pv->chapter_queue = hb_chapter_queue_init();

    err = hb_vt_settings_xlat(pv, job);
    if (err != noErr)
    {
        *job->die = 1;
        return -1;
    }

    err = hb_vt_parse_options(pv, job);
    if (err != noErr)
    {
        *job->die = 1;
        return -1;
    }

    pv->attachments = hb_vt_attachments_xlat(pv->job);
    pv->remainingPasses = job->pass_id == HB_PASS_ENCODE_ANALYSIS ? 1 : 0;

    if (job->pass_id != HB_PASS_ENCODE_FINAL)
    {
        err = hb_vt_create_cookie(w, job, pv);
        if (err != noErr)
        {
            hb_log("VTCompressionSession: Magic Cookie Error err=%"PRId64"", (int64_t)err);
            *job->die = 1;
            return -1;
        }

        // Read the actual level and tier and set
        // the Dolby Vision level and data limits
        if (job->passthru_dynamic_hdr_metadata & HB_HDR_DYNAMIC_METADATA_DOVI)
        {
            int level_idc, high_tier;
            hb_parse_h265_extradata(*w->extradata, &level_idc, &high_tier);

            int pps = (double)job->width * job->height * (job->vrate.num / job->vrate.den);
            int bitrate = job->vquality == HB_INVALID_VIDEO_QUALITY ? job->vbitrate : -1;

            // Dolby Vision requires VBV settings to enable HRD
            // set the max value for the current level or guess one
            if (pv->settings.vbv.maxrate == 0 || pv->settings.vbv.bufsize == 0)
            {
                int max_rate = hb_dovi_max_rate(job->vcodec, job->width, pps, bitrate * 1.5,
                                                level_idc, high_tier);
                pv->settings.vbv.maxrate = max_rate;
                pv->settings.vbv.bufsize = max_rate;
            }

            job->dovi.dv_level = hb_dovi_level(job->width, pps, pv->settings.vbv.maxrate, high_tier);

            // VideoToolbox CQ seems to not support data rate limits correctly,
            // just set a high enough level for now, and reset the vbv settings
            if (job->vquality != HB_INVALID_VIDEO_QUALITY)
            {
                pv->settings.vbv.maxrate = 0;
                pv->settings.vbv.bufsize = 0;
                hb_log("encvt_Init: data rate limits not supported in CQ mode, Dolby Vision file might be out of specs");
            }
            // Data limits are poorly supported in average mode too, disabling for now
            else
            {
                pv->settings.vbv.maxrate = 0;
                pv->settings.vbv.bufsize = 0;
                hb_log("encvt_Init: data rate limits not supported in ABR mode, Dolby Vision file might be out of specs");
            }
        }

        err = hb_vt_init_session(w, job, pv, 0);
        if (err != noErr)
        {
            hb_log("VTCompressionSession: Error creating a VTCompressionSession err=%"PRId64"", (int64_t)err);
            *job->die = 1;
            return -1;
        }
    }
    else
    {
        err = hb_vt_reuse_session(w, job, pv);
        if (err != noErr)
        {
            hb_log("VTCompressionSession: Error reusing a VTCompressionSession err=%"PRId64"", (int64_t)err);
            *job->die = 1;
            return -1;
        }
    }

    return 0;
}

void encvt_close(hb_work_object_t * w)
{
    hb_work_private_t *pv = w->private_data;

    if (pv == NULL)
    {
        return;
    }

    hb_chapter_queue_close(&pv->chapter_queue);

    // A cancelled encode doesn't send an EOF,
    // do some additional cleanups here
    if (*pv->job->die)
    {
        if (pv->session)
        {
            VTCompressionSessionCompleteFrames(pv->session, kCMTimeIndefinite);
        }
        if (pv->queue)
        {
            CMSampleBufferRef sampleBuffer;
            while ((sampleBuffer = (CMSampleBufferRef)CMSimpleQueueDequeue(pv->queue)))
            {
                CFRelease(sampleBuffer);
            }
        }
    }

    if (pv->remainingPasses == 0 || *pv->job->die)
    {
        if (pv->session)
        {
            VTCompressionSessionInvalidate(pv->session);
            CFRelease(pv->session);
        }
        if (pv->passStorage)
        {
            VTMultiPassStorageClose(pv->passStorage);
            CFRelease(pv->passStorage);
        }
        if (pv->queue)
        {
            CFRelease(pv->queue);
        }
        if (pv->format)
        {
            CFRelease(pv->format);
        }
    }

    if (pv->settings.color.masteringDisplay)
    {
        CFRelease(pv->settings.color.masteringDisplay);
    }
    if (pv->settings.color.contentLightLevel)
    {
        CFRelease(pv->settings.color.contentLightLevel);
    }
    if (pv->settings.color.ambientViewingEnviroment)
    {
        CFRelease(pv->settings.color.ambientViewingEnviroment);
    }
    if (pv->attachments)
    {
        CFRelease(pv->attachments);
    }

    free(pv);
    w->private_data = NULL;
}

static void hb_vt_send(hb_work_private_t *pv, hb_buffer_t *in)
{
    CVPixelBufferRef pix_buf = hb_vt_get_pix_buf(pv, in);

    if (pix_buf == NULL)
    {
        hb_buffer_close(&in);
        hb_log("VTCompressionSession: CVPixelBuffer error");
    }
    else
    {
        CFDictionaryRef frameProperties = NULL;
        if (in->s.new_chap && pv->job->chapter_markers)
        {
            // macOS Sonoma has got an unfixed bug that makes the whole
            // system crash and restart on M* Ultra if we force a keyframe
            // on the first frame. So avoid that.
            if (pv->frameno_in)
            {
                // chapters have to start with an IDR frame
                const void *keys[1] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
                const void *values[1] = { kCFBooleanTrue };

                frameProperties = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            }

            hb_chapter_enqueue(pv->chapter_queue, in);
        }

        hb_cv_set_attachments(pix_buf, pv->attachments);

        // VideoToolbox DTS are greater than PTS
        // So we remember the PTS values and compute DTS ourselves.
        hb_vt_save_frame_info(pv, in);
        hb_vt_compute_dts_offset(pv, in);
        pv->frameno_in++;

        // Send the frame to be encoded
        OSStatus err = VTCompressionSessionEncodeFrame(
                                                       pv->session,
                                                       pix_buf,
                                                       CMTimeMake(in->s.start, pv->settings.timescale),
                                                       CMTimeMake(in->s.duration, pv->settings.timescale),
                                                       frameProperties,
                                                       in,
                                                       NULL);
        CVPixelBufferRelease(pix_buf);

        if (err)
        {
            hb_log("VTCompressionSession: VTCompressionSessionEncodeFrame error");
        }

        if (frameProperties)
        {
            CFRelease(frameProperties);
        }
    }
}

static hb_buffer_t * hb_vt_receive(hb_work_private_t *pv)
{
    if (pv->frameno_in <= pv->job->areBframes)
    {
        // dts_delay not yet set. Queue up buffers till it is set
        return NULL;
    }

    CMSampleBufferRef sampleBuffer = (CMSampleBufferRef)CMSimpleQueueDequeue(pv->queue);
    hb_buffer_t      *buf_out = NULL;

    if (sampleBuffer)
    {
        buf_out = hb_vt_get_buf(sampleBuffer, pv);
        CFRelease(sampleBuffer);
    }
    return buf_out;
}

static void hb_vt_encode(hb_work_private_t *pv, hb_buffer_t *in, hb_buffer_list_t *list)
{
    hb_vt_send(pv, in);

    hb_buffer_t *out;
    while ((out = hb_vt_receive(pv)))
    {
        hb_buffer_list_append(list, out);
    }
}

static void hb_vt_flush(hb_work_private_t *pv, hb_buffer_t *in, hb_buffer_list_t *list)
{
    VTCompressionSessionCompleteFrames(pv->session, kCMTimeIndefinite);

    hb_buffer_t *out;
    while ((out = hb_vt_receive(pv)))
    {
        hb_buffer_list_append(list, out);
    }

    // Passthru the EOF to the end of the chain
    hb_buffer_list_append(list, in);
}

static void hb_vt_end_pass(hb_work_private_t *pv)
{
    if (pv->job->pass_id == HB_PASS_ENCODE_ANALYSIS)
    {
        OSStatus err = noErr;
        Boolean furtherPassesRequestedOut;
        err = VTCompressionSessionEndPass(pv->session,
                                          &furtherPassesRequestedOut,
                                          0);
        if (err != noErr)
        {
            hb_log("VTCompressionSessionEndPass error");
        }
        if (furtherPassesRequestedOut == false)
        {
            hb_log("VTCompressionSessionEndPass: no additional pass requested");
        }

        // Save the sessions and the related context for the next pass
        vt_interjob_t *context = (vt_interjob_t *)malloc(sizeof(vt_interjob_t));
        context->session     = pv->session;
        context->passStorage = pv->passStorage;
        context->queue       = pv->queue;
        context->format      = pv->format;
        context->areBframes  = pv->job->areBframes;

        hb_interjob_t *interjob = hb_interjob_get(pv->job->h);
        interjob->context = context;
    }
    else if (pv->job->pass_id == HB_PASS_ENCODE_FINAL)
    {
        VTCompressionSessionEndPass(pv->session, NULL, 0);
    }
}

int encvt_work(hb_work_object_t *w, hb_buffer_t **buf_in, hb_buffer_t **buf_out)
{
    hb_work_private_t *pv = w->private_data;
    hb_buffer_t *in = *buf_in;
    hb_buffer_list_t list;

    // Take ownership of the input buffer, avoid a memcpy
    *buf_in = NULL;
    hb_buffer_list_clear(&list);

    if (in->s.flags & HB_BUF_FLAG_EOF)
    {
        // EOF on input. Flush any frames still in the decoder then
        // send the eof downstream to tell the muxer we're done.
        hb_vt_flush(pv, in, &list);
        *buf_out = hb_buffer_list_clear(&list);

        hb_vt_end_pass(pv);

        return HB_WORK_DONE;
    }

    // Not EOF - encode the packet
    hb_vt_encode(pv, in, &list);
    *buf_out = hb_buffer_list_clear(&list);

    return HB_WORK_OK;
}
