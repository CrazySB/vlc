/*****************************************************************************
 * mediacodec_jni.c: mc_api implementation using JNI
 *****************************************************************************
 * Copyright © 2015 VLC authors and VideoLAN, VideoLabs
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <jni.h>
#include <stdint.h>
#include <assert.h>

#include <vlc_common.h>

#include <OMX_Core.h>
#include <OMX_Component.h>
#include "omxil_utils.h"

#include "mediacodec.h"

#define THREAD_NAME "mediacodec_jni"

#define BUFFER_FLAG_CODEC_CONFIG  2
#define INFO_OUTPUT_BUFFERS_CHANGED -3
#define INFO_OUTPUT_FORMAT_CHANGED  -2
#define INFO_TRY_AGAIN_LATER        -1

/*****************************************************************************
 * JNI Initialisation
 *****************************************************************************/

struct jfields
{
    jclass media_codec_list_class, media_codec_class, media_format_class;
    jclass buffer_info_class, byte_buffer_class;
    jmethodID tostring;
    jmethodID get_codec_count, get_codec_info_at, is_encoder, get_capabilities_for_type;
    jfieldID profile_levels_field, profile_field, level_field;
    jmethodID get_supported_types, get_name;
    jmethodID create_by_codec_name, configure, start, stop, flush, release;
    jmethodID get_output_format;
    jmethodID get_input_buffers, get_input_buffer;
    jmethodID get_output_buffers, get_output_buffer;
    jmethodID dequeue_input_buffer, dequeue_output_buffer, queue_input_buffer;
    jmethodID release_output_buffer;
    jmethodID create_video_format, create_audio_format;
    jmethodID set_integer, set_bytebuffer, get_integer;
    jmethodID buffer_info_ctor;
    jfieldID size_field, offset_field, pts_field;
};
static struct jfields jfields;

enum Types
{
    METHOD, STATIC_METHOD, FIELD
};

#define OFF(x) offsetof(struct jfields, x)
struct classname
{
    const char *name;
    int offset;
};
static const struct classname classes[] = {
    { "android/media/MediaCodecList", OFF(media_codec_list_class) },
    { "android/media/MediaCodec", OFF(media_codec_class) },
    { "android/media/MediaFormat", OFF(media_format_class) },
    { "android/media/MediaFormat", OFF(media_format_class) },
    { "android/media/MediaCodec$BufferInfo", OFF(buffer_info_class) },
    { "java/nio/ByteBuffer", OFF(byte_buffer_class) },
    { NULL, 0 },
};

struct member
{
    const char *name;
    const char *sig;
    const char *class;
    int offset;
    int type;
    bool critical;
};
static const struct member members[] = {
    { "toString", "()Ljava/lang/String;", "java/lang/Object", OFF(tostring), METHOD, true },

    { "getCodecCount", "()I", "android/media/MediaCodecList", OFF(get_codec_count), STATIC_METHOD, true },
    { "getCodecInfoAt", "(I)Landroid/media/MediaCodecInfo;", "android/media/MediaCodecList", OFF(get_codec_info_at), STATIC_METHOD, true },

    { "isEncoder", "()Z", "android/media/MediaCodecInfo", OFF(is_encoder), METHOD, true },
    { "getSupportedTypes", "()[Ljava/lang/String;", "android/media/MediaCodecInfo", OFF(get_supported_types), METHOD, true },
    { "getName", "()Ljava/lang/String;", "android/media/MediaCodecInfo", OFF(get_name), METHOD, true },
    { "getCapabilitiesForType", "(Ljava/lang/String;)Landroid/media/MediaCodecInfo$CodecCapabilities;", "android/media/MediaCodecInfo", OFF(get_capabilities_for_type), METHOD, true },

    { "profileLevels", "[Landroid/media/MediaCodecInfo$CodecProfileLevel;", "android/media/MediaCodecInfo$CodecCapabilities", OFF(profile_levels_field), FIELD, true },
    { "profile", "I", "android/media/MediaCodecInfo$CodecProfileLevel", OFF(profile_field), FIELD, true },
    { "level", "I", "android/media/MediaCodecInfo$CodecProfileLevel", OFF(level_field), FIELD, true },

    { "createByCodecName", "(Ljava/lang/String;)Landroid/media/MediaCodec;", "android/media/MediaCodec", OFF(create_by_codec_name), STATIC_METHOD, true },
    { "configure", "(Landroid/media/MediaFormat;Landroid/view/Surface;Landroid/media/MediaCrypto;I)V", "android/media/MediaCodec", OFF(configure), METHOD, true },
    { "start", "()V", "android/media/MediaCodec", OFF(start), METHOD, true },
    { "stop", "()V", "android/media/MediaCodec", OFF(stop), METHOD, true },
    { "flush", "()V", "android/media/MediaCodec", OFF(flush), METHOD, true },
    { "release", "()V", "android/media/MediaCodec", OFF(release), METHOD, true },
    { "getOutputFormat", "()Landroid/media/MediaFormat;", "android/media/MediaCodec", OFF(get_output_format), METHOD, true },
    { "getInputBuffers", "()[Ljava/nio/ByteBuffer;", "android/media/MediaCodec", OFF(get_input_buffers), METHOD, false },
    { "getInputBuffer", "(I)Ljava/nio/ByteBuffer;", "android/media/MediaCodec", OFF(get_input_buffer), METHOD, false },
    { "getOutputBuffers", "()[Ljava/nio/ByteBuffer;", "android/media/MediaCodec", OFF(get_output_buffers), METHOD, false },
    { "getOutputBuffer", "(I)Ljava/nio/ByteBuffer;", "android/media/MediaCodec", OFF(get_output_buffer), METHOD, false },
    { "dequeueInputBuffer", "(J)I", "android/media/MediaCodec", OFF(dequeue_input_buffer), METHOD, true },
    { "dequeueOutputBuffer", "(Landroid/media/MediaCodec$BufferInfo;J)I", "android/media/MediaCodec", OFF(dequeue_output_buffer), METHOD, true },
    { "queueInputBuffer", "(IIIJI)V", "android/media/MediaCodec", OFF(queue_input_buffer), METHOD, true },
    { "releaseOutputBuffer", "(IZ)V", "android/media/MediaCodec", OFF(release_output_buffer), METHOD, true },

    { "createVideoFormat", "(Ljava/lang/String;II)Landroid/media/MediaFormat;", "android/media/MediaFormat", OFF(create_video_format), STATIC_METHOD, true },
    { "createAudioFormat", "(Ljava/lang/String;II)Landroid/media/MediaFormat;", "android/media/MediaFormat", OFF(create_audio_format), STATIC_METHOD, true },
    { "setInteger", "(Ljava/lang/String;I)V", "android/media/MediaFormat", OFF(set_integer), METHOD, true },
    { "getInteger", "(Ljava/lang/String;)I", "android/media/MediaFormat", OFF(get_integer), METHOD, true },
    { "setByteBuffer", "(Ljava/lang/String;Ljava/nio/ByteBuffer;)V", "android/media/MediaFormat", OFF(set_bytebuffer), METHOD, true },

    { "<init>", "()V", "android/media/MediaCodec$BufferInfo", OFF(buffer_info_ctor), METHOD, true },
    { "size", "I", "android/media/MediaCodec$BufferInfo", OFF(size_field), FIELD, true },
    { "offset", "I", "android/media/MediaCodec$BufferInfo", OFF(offset_field), FIELD, true },
    { "presentationTimeUs", "J", "android/media/MediaCodec$BufferInfo", OFF(pts_field), FIELD, true },
    { NULL, NULL, NULL, 0, 0, false },
};

static int jstrcmp(JNIEnv* env, jobject str, const char* str2)
{
    jsize len = (*env)->GetStringUTFLength(env, str);
    if (len != (jsize) strlen(str2))
        return -1;
    const char *ptr = (*env)->GetStringUTFChars(env, str, NULL);
    int ret = memcmp(ptr, str2, len);
    (*env)->ReleaseStringUTFChars(env, str, ptr);
    return ret;
}

static inline bool check_exception(JNIEnv *env)
{
    if ((*env)->ExceptionOccurred(env))
    {
        (*env)->ExceptionClear(env);
        return true;
    }
    else
        return false;
}
#define CHECK_EXCEPTION() check_exception( env )
#define GET_ENV() if (!(env = android_getEnv(api->p_obj, THREAD_NAME))) return VLC_EGENERIC;

static inline int get_integer(JNIEnv *env, jobject obj, const char *psz_name)
{
    jstring jname = (*env)->NewStringUTF(env, psz_name);
    if (!CHECK_EXCEPTION() && jname)
    {
        int i_ret = (*env)->CallIntMethod(env, obj, jfields.get_integer, jname);
        (*env)->DeleteLocalRef(env, jname);
        /* getInteger can throw NullPointerException (when fetching the
         * "channel-mask" property for example) */
        if (CHECK_EXCEPTION())
            return 0;
        return i_ret;
    }
    else
        return 0;
}
#define GET_INTEGER(obj, name) get_integer(env, obj, name)

/* Initialize all jni fields.
 * Done only one time during the first initialisation */
static bool
InitJNIFields (vlc_object_t *p_obj, JNIEnv *env)
{
    static vlc_mutex_t lock = VLC_STATIC_MUTEX;
    static int i_init_state = -1;
    bool ret;

    vlc_mutex_lock( &lock );

    if( i_init_state != -1 )
        goto end;

    i_init_state = 0;

    for (int i = 0; classes[i].name; i++)
    {
        jclass clazz = (*env)->FindClass(env, classes[i].name);
        if (CHECK_EXCEPTION())
        {
            msg_Warn(p_obj, "Unable to find class %s", classes[i].name);
            goto end;
        }
        *(jclass*)((uint8_t*)&jfields + classes[i].offset) =
            (jclass) (*env)->NewGlobalRef(env, clazz);
    }

    jclass last_class;
    for (int i = 0; members[i].name; i++)
    {
        if (i == 0 || strcmp(members[i].class, members[i - 1].class))
            last_class = (*env)->FindClass(env, members[i].class);

        if (CHECK_EXCEPTION())
        {
            msg_Warn(p_obj, "Unable to find class %s", members[i].class);
            goto end;
        }

        switch (members[i].type) {
        case METHOD:
            *(jmethodID*)((uint8_t*)&jfields + members[i].offset) =
                (*env)->GetMethodID(env, last_class, members[i].name, members[i].sig);
            break;
        case STATIC_METHOD:
            *(jmethodID*)((uint8_t*)&jfields + members[i].offset) =
                (*env)->GetStaticMethodID(env, last_class, members[i].name, members[i].sig);
            break;
        case FIELD:
            *(jfieldID*)((uint8_t*)&jfields + members[i].offset) =
                (*env)->GetFieldID(env, last_class, members[i].name, members[i].sig);
            break;
        }
        if (CHECK_EXCEPTION())
        {
            msg_Warn(p_obj, "Unable to find the member %s in %s",
                     members[i].name, members[i].class);
            if (members[i].critical)
                goto end;
        }
    }
    /* getInputBuffers and getOutputBuffers are deprecated if API >= 21
     * use getInputBuffer and getOutputBuffer instead. */
    if (jfields.get_input_buffer && jfields.get_output_buffer)
    {
        jfields.get_output_buffers =
        jfields.get_input_buffers = NULL;
    }
    else if (!jfields.get_output_buffers && !jfields.get_input_buffers)
    {
        msg_Err(p_obj, "Unable to find get Output/Input Buffer/Buffers");
        goto end;
    }

    i_init_state = 1;
end:
    ret = i_init_state == 1;
    if( !ret )
        msg_Err(p_obj, "MediaCodec jni init failed");

    vlc_mutex_unlock( &lock );
    return ret;
}

/****************************************************************************
 * Local prototypes
 ****************************************************************************/

struct mc_api_sys
{
    jobject codec;
    jobject buffer_info;
    jobject input_buffers, output_buffers;
};

/*****************************************************************************
 * MediaCodec_GetName
 *****************************************************************************/
char* MediaCodec_GetName(vlc_object_t *p_obj, const char *psz_mime,
                         size_t h264_profile)
{
    JNIEnv *env;
    int num_codecs;
    jstring jmime;
    char *psz_name = NULL;

    if (!(env = android_getEnv(p_obj, THREAD_NAME)))
        return NULL;

    if (!InitJNIFields(p_obj, env))
        return NULL;

    jmime = (*env)->NewStringUTF(env, psz_mime);
    if (!jmime)
        return NULL;

    num_codecs = (*env)->CallStaticIntMethod(env,
                                             jfields.media_codec_list_class,
                                             jfields.get_codec_count);

    for (int i = 0; i < num_codecs; i++)
    {
        jobject codec_capabilities = NULL;
        jobject profile_levels = NULL;
        jobject info = NULL;
        jobject name = NULL;
        jobject types = NULL;
        jsize name_len = 0;
        int profile_levels_len = 0, num_types = 0;
        const char *name_ptr = NULL;
        bool found = false;

        info = (*env)->CallStaticObjectMethod(env, jfields.media_codec_list_class,
                                              jfields.get_codec_info_at, i);

        name = (*env)->CallObjectMethod(env, info, jfields.get_name);
        name_len = (*env)->GetStringUTFLength(env, name);
        name_ptr = (*env)->GetStringUTFChars(env, name, NULL);

        if (OMXCodec_IsBlacklisted( name_ptr, name_len))
            goto loopclean;

        if ((*env)->CallBooleanMethod(env, info, jfields.is_encoder))
            goto loopclean;

        codec_capabilities = (*env)->CallObjectMethod(env, info,
                                                      jfields.get_capabilities_for_type,
                                                      jmime);
        if (CHECK_EXCEPTION())
        {
            msg_Warn(p_obj, "Exception occurred in MediaCodecInfo.getCapabilitiesForType");
            goto loopclean;
        }
        else if (codec_capabilities)
        {
            profile_levels = (*env)->GetObjectField(env, codec_capabilities, jfields.profile_levels_field);
            if (profile_levels)
                profile_levels_len = (*env)->GetArrayLength(env, profile_levels);
        }
        msg_Dbg(p_obj, "Number of profile levels: %d", profile_levels_len);

        types = (*env)->CallObjectMethod(env, info, jfields.get_supported_types);
        num_types = (*env)->GetArrayLength(env, types);
        found = false;

        for (int j = 0; j < num_types && !found; j++)
        {
            jobject type = (*env)->GetObjectArrayElement(env, types, j);
            if (!jstrcmp(env, type, psz_mime))
            {
                /* The mime type is matching for this component. We
                   now check if the capabilities of the codec is
                   matching the video format. */
                if (h264_profile)
                {
                    /* This decoder doesn't expose its profiles and is high
                     * profile capable */
                    if (!strncmp(name_ptr, "OMX.LUMEVideoDecoder", __MIN(20, name_len)))
                        found = true;

                    for (int i = 0; i < profile_levels_len && !found; ++i)
                    {
                        jobject profile_level = (*env)->GetObjectArrayElement(env, profile_levels, i);

                        int omx_profile = (*env)->GetIntField(env, profile_level, jfields.profile_field);
                        size_t codec_profile = convert_omx_to_profile_idc(omx_profile);
                        (*env)->DeleteLocalRef(env, profile_level);
                        if (codec_profile != h264_profile)
                            continue;
                        /* Some encoders set the level too high, thus we ignore it for the moment.
                           We could try to guess the actual profile based on the resolution. */
                        found = true;
                    }
                }
                else
                    found = true;
            }
            (*env)->DeleteLocalRef(env, type);
        }
        if (found)
        {
            msg_Dbg(p_obj, "using %.*s", name_len, name_ptr);
            psz_name = malloc(name_len + 1);
            if (psz_name)
            {
                memcpy(psz_name, name_ptr, name_len);
                psz_name[name_len] = '\0';
            }
        }
loopclean:
        if (name)
        {
            (*env)->ReleaseStringUTFChars(env, name, name_ptr);
            (*env)->DeleteLocalRef(env, name);
        }
        if (profile_levels)
            (*env)->DeleteLocalRef(env, profile_levels);
        if (types)
            (*env)->DeleteLocalRef(env, types);
        if (codec_capabilities)
            (*env)->DeleteLocalRef(env, codec_capabilities);
        if (info)
            (*env)->DeleteLocalRef(env, info);
        if (found)
            break;
    }
    (*env)->DeleteLocalRef(env, jmime);

    return psz_name;
}

/*****************************************************************************
 * Stop
 *****************************************************************************/
static int Stop(mc_api *api)
{
    mc_api_sys *p_sys = api->p_sys;
    JNIEnv *env;

    api->b_direct_rendering = false;

    GET_ENV();

    if (p_sys->input_buffers)
    {
        (*env)->DeleteGlobalRef(env, p_sys->input_buffers);
        p_sys->input_buffers = NULL;
    }
    if (p_sys->output_buffers)
    {
        (*env)->DeleteGlobalRef(env, p_sys->output_buffers);
        p_sys->output_buffers = NULL;
    }
    if (p_sys->codec)
    {
        if (api->b_started)
        {
            (*env)->CallVoidMethod(env, p_sys->codec, jfields.stop);
            if (CHECK_EXCEPTION())
                msg_Err(api->p_obj, "Exception in MediaCodec.stop");
            api->b_started = false;
        }

        (*env)->CallVoidMethod(env, p_sys->codec, jfields.release);
        if (CHECK_EXCEPTION())
            msg_Err(api->p_obj, "Exception in MediaCodec.release");
        (*env)->DeleteGlobalRef(env, p_sys->codec);
        p_sys->codec = NULL;
    }
    if (p_sys->buffer_info)
    {
        (*env)->DeleteGlobalRef(env, p_sys->buffer_info);
        p_sys->buffer_info = NULL;
    }
    msg_Dbg(api->p_obj, "MediaCodec via JNI closed");
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Start
 *****************************************************************************/
static int Start(mc_api *api, const char *psz_name, const char *psz_mime,
                 union mc_api_args *p_args)
{
    mc_api_sys *p_sys = api->p_sys;
    JNIEnv* env = NULL;
    int i_ret = VLC_EGENERIC;
    bool b_direct_rendering = false;
    jstring jmime = NULL;
    jstring jcodec_name = NULL;
    jobject jcodec = NULL;
    jobject jformat = NULL;
    jstring jrotation_string = NULL;
    jobject jinput_buffers = NULL;
    jobject joutput_buffers = NULL;
    jobject jbuffer_info = NULL;
    jobject jsurface = NULL;

    GET_ENV();

    jmime = (*env)->NewStringUTF(env, psz_mime);
    jcodec_name = (*env)->NewStringUTF(env, psz_name);
    if (!jmime || !jcodec_name)
        goto error;

    /* This method doesn't handle errors nicely, it crashes if the codec isn't
     * found.  (The same goes for createDecoderByType.) This is fixed in latest
     * AOSP and in 4.2, but not in 4.1 devices. */
    jcodec = (*env)->CallStaticObjectMethod(env, jfields.media_codec_class,
                                            jfields.create_by_codec_name,
                                            jcodec_name);
    if (CHECK_EXCEPTION())
    {
        msg_Warn(api->p_obj, "Exception occurred in MediaCodec.createByCodecName");
        goto error;
    }
    p_sys->codec = (*env)->NewGlobalRef(env, jcodec);

    if (api->b_video)
    {
        jformat = (*env)->CallStaticObjectMethod(env,
                                                 jfields.media_format_class,
                                                 jfields.create_video_format,
                                                 jmime,
                                                 p_args->video.i_width,
                                                 p_args->video.i_height);
        if (p_args->video.p_awh)
            jsurface = AWindowHandler_getSurface(p_args->video.p_awh,
                                                 AWindow_Video);
        b_direct_rendering = !!jsurface;

        /* There is no way to rotate the video using direct rendering (and
         * using a SurfaceView) before  API 21 (Lollipop). Therefore, we
         * deactivate direct rendering if video doesn't have a normal rotation
         * and if get_input_buffer method is not present (This method exists
         * since API 21). */
        if (b_direct_rendering && p_args->video.i_angle != 0
         && !jfields.get_input_buffer)
            b_direct_rendering = false;

        if (b_direct_rendering && p_args->video.i_angle != 0)
            jrotation_string = (*env)->NewStringUTF(env, "rotation-degrees");
            (*env)->CallVoidMethod(env, jformat, jfields.set_integer,
                                   jrotation_string, p_args->video.i_angle);
    }
    else
    {
        jformat = (*env)->CallStaticObjectMethod(env,
                                                 jfields.media_format_class,
                                                 jfields.create_audio_format,
                                                 jmime,
                                                 p_args->audio.i_sample_rate,
                                                 p_args->audio.i_channel_count);
    }

    if (b_direct_rendering)
    {
        // Configure MediaCodec with the Android surface.
        (*env)->CallVoidMethod(env, p_sys->codec, jfields.configure,
                               jformat, jsurface, NULL, 0);
        if (CHECK_EXCEPTION())
        {
            msg_Warn(api->p_obj, "Exception occurred in MediaCodec.configure "
                                 "with an output surface.");
            goto error;
        }
    }
    else
    {
        (*env)->CallVoidMethod(env, p_sys->codec, jfields.configure,
                               jformat, NULL, NULL, 0);
        if (CHECK_EXCEPTION())
        {
            msg_Warn(api->p_obj, "Exception occurred in MediaCodec.configure");
            goto error;
        }
    }

    (*env)->CallVoidMethod(env, p_sys->codec, jfields.start);
    if (CHECK_EXCEPTION())
    {
        msg_Warn(api->p_obj, "Exception occurred in MediaCodec.start");
        goto error;
    }
    api->b_started = true;

    if (jfields.get_input_buffers && jfields.get_output_buffers)
    {

        jinput_buffers = (*env)->CallObjectMethod(env, p_sys->codec,
                                                  jfields.get_input_buffers);
        if (CHECK_EXCEPTION())
        {
            msg_Err(api->p_obj, "Exception in MediaCodec.getInputBuffers");
            goto error;
        }
        p_sys->input_buffers = (*env)->NewGlobalRef(env, jinput_buffers);

        joutput_buffers = (*env)->CallObjectMethod(env, p_sys->codec,
                                                   jfields.get_output_buffers);
        if (CHECK_EXCEPTION())
        {
            msg_Err(api->p_obj, "Exception in MediaCodec.getOutputBuffers");
            goto error;
        }
        p_sys->output_buffers = (*env)->NewGlobalRef(env, joutput_buffers);
    }
    jbuffer_info = (*env)->NewObject(env, jfields.buffer_info_class,
                                     jfields.buffer_info_ctor);
    p_sys->buffer_info = (*env)->NewGlobalRef(env, jbuffer_info);

    api->b_direct_rendering = b_direct_rendering;
    i_ret = VLC_SUCCESS;
    msg_Dbg(api->p_obj, "MediaCodec via JNI opened");

error:
    if (jmime)
        (*env)->DeleteLocalRef(env, jmime);
    if (jcodec_name)
        (*env)->DeleteLocalRef(env, jcodec_name);
    if (jcodec)
        (*env)->DeleteLocalRef(env, jcodec);
    if (jformat)
        (*env)->DeleteLocalRef(env, jformat);
    if (jrotation_string)
        (*env)->DeleteLocalRef(env, jrotation_string);
    if (jinput_buffers)
        (*env)->DeleteLocalRef(env, jinput_buffers);
    if (joutput_buffers)
        (*env)->DeleteLocalRef(env, joutput_buffers);
    if (jbuffer_info)
        (*env)->DeleteLocalRef(env, jbuffer_info);

    if (i_ret != VLC_SUCCESS)
        Stop(api);
    return i_ret;
}

/*****************************************************************************
 * Flush
 *****************************************************************************/
static int Flush(mc_api *api)
{
    mc_api_sys *p_sys = api->p_sys;
    JNIEnv *env = NULL;

    GET_ENV();

    (*env)->CallVoidMethod(env, p_sys->codec, jfields.flush);
    if (CHECK_EXCEPTION())
    {
        msg_Warn(api->p_obj, "Exception occurred in MediaCodec.flush");
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * PutInput
 *****************************************************************************/
static int PutInput(mc_api *api, const void *p_buf, size_t i_size,
                    mtime_t i_ts, bool b_config, mtime_t i_timeout)
{
    mc_api_sys *p_sys = api->p_sys;
    JNIEnv *env;
    int index;
    uint8_t *p_mc_buf;
    jobject j_mc_buf;
    jsize j_mc_size;
    jint jflags = b_config ? BUFFER_FLAG_CODEC_CONFIG : 0;

    GET_ENV();

    index = (*env)->CallIntMethod(env, p_sys->codec,
                                  jfields.dequeue_input_buffer, i_timeout);
    if (CHECK_EXCEPTION())
    {
        msg_Err(api->p_obj, "Exception occurred in MediaCodec.dequeueInputBuffer");
        return VLC_EGENERIC;
    }
    if (index < 0)
        return 0;

    if (jfields.get_input_buffers)
        j_mc_buf = (*env)->GetObjectArrayElement(env, p_sys->input_buffers,
                                                 index);
    else
    {
        j_mc_buf = (*env)->CallObjectMethod(env, p_sys->codec,
                                            jfields.get_input_buffer, index);
        if (CHECK_EXCEPTION())
        {
            msg_Err(api->p_obj, "Exception in MediaCodec.getInputBuffer");
            return VLC_EGENERIC;
        }
    }
    j_mc_size = (*env)->GetDirectBufferCapacity(env, j_mc_buf);
    p_mc_buf = (*env)->GetDirectBufferAddress(env, j_mc_buf);
    if (j_mc_size < 0)
    {
        msg_Err(api->p_obj, "Java buffer has invalid size");
        (*env)->DeleteLocalRef(env, j_mc_buf);
        return VLC_EGENERIC;
    }
    if ((size_t) j_mc_size > i_size)
        j_mc_size = i_size;
    memcpy(p_mc_buf, p_buf, j_mc_size);

    (*env)->CallVoidMethod(env, p_sys->codec, jfields.queue_input_buffer,
                           index, 0, j_mc_size, i_ts, jflags);
    (*env)->DeleteLocalRef(env, j_mc_buf);
    if (CHECK_EXCEPTION())
    {
        msg_Err(api->p_obj, "Exception in MediaCodec.queueInputBuffer");
        return VLC_EGENERIC;
    }

    return 1;
}

/*****************************************************************************
 * GetOutput
 *****************************************************************************/
static int GetOutput(mc_api *api, mc_api_out *p_out, mtime_t i_timeout)
{
    mc_api_sys *p_sys = api->p_sys;
    JNIEnv *env;
    int i_index;

    GET_ENV();
    i_index = (*env)->CallIntMethod(env, p_sys->codec,
                                    jfields.dequeue_output_buffer,
                                    p_sys->buffer_info, i_timeout);
    if (CHECK_EXCEPTION())
    {
        msg_Err(api->p_obj, "Exception in MediaCodec.dequeueOutputBuffer");
        return VLC_EGENERIC;
    }

    if (i_index >= 0)
    {
        p_out->type = MC_OUT_TYPE_BUF;
        p_out->u.buf.i_index = i_index;
        p_out->u.buf.i_ts = (*env)->GetLongField(env, p_sys->buffer_info,
                                                 jfields.pts_field);

        if (api->b_direct_rendering)
        {
            p_out->u.buf.p_ptr = NULL;
            p_out->u.buf.i_size = 0;
        }
        else
        {
            jobject buf;
            uint8_t *ptr;
            int offset;

            if (jfields.get_output_buffers)
                buf = (*env)->GetObjectArrayElement(env, p_sys->output_buffers,
                                                    i_index);
            else
            {
                buf = (*env)->CallObjectMethod(env, p_sys->codec,
                                               jfields.get_output_buffer,
                                               i_index);
                if (CHECK_EXCEPTION())
                {
                    msg_Err(api->p_obj, "Exception in MediaCodec.getOutputBuffer");
                    return VLC_EGENERIC;
                }
            }
            //jsize buf_size = (*env)->GetDirectBufferCapacity(env, buf);
            ptr = (*env)->GetDirectBufferAddress(env, buf);

            offset = (*env)->GetIntField(env, p_sys->buffer_info,
                                         jfields.offset_field);
            p_out->u.buf.p_ptr = ptr + offset;
            p_out->u.buf.i_size = (*env)->GetIntField(env, p_sys->buffer_info,
                                                       jfields.size_field);
            (*env)->DeleteLocalRef(env, buf);
        }
        return 1;
    }
    else if (i_index == INFO_OUTPUT_FORMAT_CHANGED)
    {
        jobject format = NULL;
        jobject format_string = NULL;
        jsize format_len;
        const char *format_ptr;

        format = (*env)->CallObjectMethod(env, p_sys->codec,
                                          jfields.get_output_format);
        if (CHECK_EXCEPTION())
        {
            msg_Err(api->p_obj, "Exception in MediaCodec.getOutputFormat");
            return VLC_EGENERIC;
        }

        format_string = (*env)->CallObjectMethod(env, format, jfields.tostring);

        format_len = (*env)->GetStringUTFLength(env, format_string);
        format_ptr = (*env)->GetStringUTFChars(env, format_string, NULL);
        msg_Dbg(api->p_obj, "output format changed: %.*s", format_len,
                format_ptr);
        (*env)->ReleaseStringUTFChars(env, format_string, format_ptr);

        p_out->type = MC_OUT_TYPE_CONF;
        if (api->b_video)
        {
            p_out->u.conf.video.width         = GET_INTEGER(format, "width");
            p_out->u.conf.video.height        = GET_INTEGER(format, "height");
            p_out->u.conf.video.stride        = GET_INTEGER(format, "stride");
            p_out->u.conf.video.slice_height  = GET_INTEGER(format, "slice-height");
            p_out->u.conf.video.pixel_format  = GET_INTEGER(format, "color-format");
            p_out->u.conf.video.crop_left     = GET_INTEGER(format, "crop-left");
            p_out->u.conf.video.crop_top      = GET_INTEGER(format, "crop-top");
            p_out->u.conf.video.crop_right    = GET_INTEGER(format, "crop-right");
            p_out->u.conf.video.crop_bottom   = GET_INTEGER(format, "crop-bottom");
        }
        else
        {
            p_out->u.conf.audio.channel_count = GET_INTEGER(format, "channel-count");
            p_out->u.conf.audio.channel_mask = GET_INTEGER(format, "channel-mask");
            p_out->u.conf.audio.sample_rate = GET_INTEGER(format, "sample-rate");
        }

        (*env)->DeleteLocalRef(env, format);
        return 1;
    }
    else if (i_index == INFO_OUTPUT_BUFFERS_CHANGED)
    {
        jobject joutput_buffers;

        msg_Dbg(api->p_obj, "output buffers changed");
        if (!jfields.get_output_buffers)
            return 0;
        (*env)->DeleteGlobalRef(env, p_sys->output_buffers);

        joutput_buffers = (*env)->CallObjectMethod(env, p_sys->codec,
                                                   jfields.get_output_buffers);
        if (CHECK_EXCEPTION())
        {
            msg_Err(api->p_obj, "Exception in MediaCodec.getOutputBuffer");
            p_sys->output_buffers = NULL;
            return VLC_EGENERIC;
        }
        p_sys->output_buffers = (*env)->NewGlobalRef(env, joutput_buffers);
        (*env)->DeleteLocalRef(env, joutput_buffers);
    }
    return 0;
}

/*****************************************************************************
 * ReleaseOutput
 *****************************************************************************/
static int ReleaseOutput(mc_api *api, int i_index, bool b_render)
{
    mc_api_sys *p_sys = api->p_sys;
    JNIEnv *env;

    GET_ENV();

    (*env)->CallVoidMethod(env, p_sys->codec, jfields.release_output_buffer,
                           i_index, b_render);
    if (CHECK_EXCEPTION())
    {
        msg_Err(api->p_obj, "Exception in MediaCodec.releaseOutputBuffer");
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Clean
 *****************************************************************************/
static void Clean(mc_api *api)
{
    free(api->p_sys);
}

/*****************************************************************************
 * MediaCodecJni_New
 *****************************************************************************/
int MediaCodecJni_Init(mc_api *api)
{
    JNIEnv *env;

    GET_ENV();

    if (!InitJNIFields(api->p_obj, env))
        return VLC_EGENERIC;

    api->p_sys = calloc(1, sizeof(mc_api_sys));
    if (!api->p_sys)
        return VLC_EGENERIC;

    api->clean = Clean;
    api->start = Start;
    api->stop = Stop;
    api->flush = Flush;
    api->put_in = PutInput;
    api->get_out = GetOutput;
    api->release_out = ReleaseOutput;

    /* Allow interlaced picture only after API 21 */
    api->b_support_interlaced = jfields.get_input_buffer
                                && jfields.get_output_buffer;
    return VLC_SUCCESS;
}
