/**
 * native-bridge.cpp - JNI bridge between Kotlin and the C audio engine
 *
 * This file maps Kotlin JNI calls to the C audio engine API,
 * and wraps the Oboe audio I/O layer.
 */

#include <jni.h>
#include <android/log.h>
#include <string>
#include <memory>

extern "C" {
#include "audio_engine.h"
}
#include "oboe-audio-io.h"

#define LOG_TAG "NativeBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Global Oboe audio I/O instance (one per engine)
static std::unique_ptr<OboeAudioIO> g_audioIO;

// Global JVM reference for callbacks from native threads
static JavaVM *g_jvm = nullptr;
static jobject g_eventListener = nullptr;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void * /*reserved*/) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

// Helper to get JNIEnv from any thread
static JNIEnv *getEnv() {
    JNIEnv *env = nullptr;
    if (g_jvm) {
        g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
        if (!env) {
            g_jvm->AttachCurrentThread(&env, nullptr);
        }
    }
    return env;
}

// Event callback from the C engine → JNI → Kotlin
static void eventCallback(const ae_event_t *event, void * /*user_data*/) {
    JNIEnv *env = getEnv();
    if (!env || !g_eventListener) return;

    jclass cls = env->GetObjectClass(g_eventListener);
    if (!cls) return;

    switch (event->type) {
        case AE_EVENT_CONNECTED: {
            jmethodID mid = env->GetMethodID(cls, "onConnected", "()V");
            if (mid) env->CallVoidMethod(g_eventListener, mid);
            break;
        }
        case AE_EVENT_DISCONNECTED: {
            jmethodID mid = env->GetMethodID(cls, "onDisconnected", "()V");
            if (mid) env->CallVoidMethod(g_eventListener, mid);
            break;
        }
        case AE_EVENT_USER_JOINED: {
            jmethodID mid = env->GetMethodID(cls, "onUserJoined",
                "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
            if (mid) {
                jstring chId = env->NewStringUTF(event->channel_id ? event->channel_id : "");
                jstring clId = env->NewStringUTF(event->client_id ? event->client_id : "");
                jstring name = env->NewStringUTF(event->user_name ? event->user_name : "");
                env->CallVoidMethod(g_eventListener, mid, chId, clId, name);
                env->DeleteLocalRef(chId);
                env->DeleteLocalRef(clId);
                env->DeleteLocalRef(name);
            }
            break;
        }
        case AE_EVENT_USER_LEFT: {
            jmethodID mid = env->GetMethodID(cls, "onUserLeft",
                "(Ljava/lang/String;Ljava/lang/String;)V");
            if (mid) {
                jstring chId = env->NewStringUTF(event->channel_id ? event->channel_id : "");
                jstring clId = env->NewStringUTF(event->client_id ? event->client_id : "");
                env->CallVoidMethod(g_eventListener, mid, chId, clId);
                env->DeleteLocalRef(chId);
                env->DeleteLocalRef(clId);
            }
            break;
        }
        case AE_EVENT_USER_SPEAKING: {
            jmethodID mid = env->GetMethodID(cls, "onUserSpeaking",
                "(Ljava/lang/String;Ljava/lang/String;)V");
            if (mid) {
                jstring chId = env->NewStringUTF(event->channel_id ? event->channel_id : "");
                jstring clId = env->NewStringUTF(event->client_id ? event->client_id : "");
                env->CallVoidMethod(g_eventListener, mid, chId, clId);
                env->DeleteLocalRef(chId);
                env->DeleteLocalRef(clId);
            }
            break;
        }
        case AE_EVENT_USER_STOPPED: {
            jmethodID mid = env->GetMethodID(cls, "onUserStopped",
                "(Ljava/lang/String;Ljava/lang/String;)V");
            if (mid) {
                jstring chId = env->NewStringUTF(event->channel_id ? event->channel_id : "");
                jstring clId = env->NewStringUTF(event->client_id ? event->client_id : "");
                env->CallVoidMethod(g_eventListener, mid, chId, clId);
                env->DeleteLocalRef(chId);
                env->DeleteLocalRef(clId);
            }
            break;
        }
        case AE_EVENT_ERROR: {
            jmethodID mid = env->GetMethodID(cls, "onError", "(Ljava/lang/String;)V");
            if (mid) {
                jstring msg = env->NewStringUTF(event->message ? event->message : "Unknown error");
                env->CallVoidMethod(g_eventListener, mid, msg);
                env->DeleteLocalRef(msg);
            }
            break;
        }
        default:
            break;
    }
    env->DeleteLocalRef(cls);
}

// ─── JNI Function Implementations ────────────────────────────────────

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeCreate(JNIEnv * /*env*/, jobject /*thiz*/) {
    ae_config_t config = ae_config_default();
    ae_engine_t *engine = ae_engine_create(&config);
    if (engine) {
        ae_engine_set_event_callback(engine, eventCallback, nullptr);
        LOGI("Engine created: %p", engine);
    }
    return reinterpret_cast<jlong>(engine);
}

JNIEXPORT void JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeDestroy(JNIEnv *env, jobject /*thiz*/, jlong ptr) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    if (engine) {
        g_audioIO.reset();
        ae_engine_destroy(engine);
        if (g_eventListener) {
            env->DeleteGlobalRef(g_eventListener);
            g_eventListener = nullptr;
        }
        LOGI("Engine destroyed");
    }
}

JNIEXPORT jint JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeConnect(
    JNIEnv *env, jobject /*thiz*/, jlong ptr,
    jstring host, jint udpPort, jint wsPort, jstring token) {

    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    const char *hostStr = env->GetStringUTFChars(host, nullptr);
    const char *tokenStr = env->GetStringUTFChars(token, nullptr);

    int result = ae_engine_connect(engine, hostStr, udpPort, wsPort, tokenStr);

    env->ReleaseStringUTFChars(host, hostStr);
    env->ReleaseStringUTFChars(token, tokenStr);
    return result;
}

JNIEXPORT void JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeDisconnect(JNIEnv * /*env*/, jobject /*thiz*/, jlong ptr) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    ae_engine_disconnect(engine);
}

JNIEXPORT jboolean JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeIsConnected(JNIEnv * /*env*/, jobject /*thiz*/, jlong ptr) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    return ae_engine_is_connected(engine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeJoinChannel(
    JNIEnv *env, jobject /*thiz*/, jlong ptr, jstring channelId) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    const char *chId = env->GetStringUTFChars(channelId, nullptr);
    int result = ae_engine_join_channel(engine, chId);
    env->ReleaseStringUTFChars(channelId, chId);
    return result;
}

JNIEXPORT jint JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeLeaveChannel(
    JNIEnv *env, jobject /*thiz*/, jlong ptr, jstring channelId) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    const char *chId = env->GetStringUTFChars(channelId, nullptr);
    int result = ae_engine_leave_channel(engine, chId);
    env->ReleaseStringUTFChars(channelId, chId);
    return result;
}

JNIEXPORT void JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeLeaveAllChannels(JNIEnv * /*env*/, jobject /*thiz*/, jlong ptr) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    ae_engine_leave_all_channels(engine);
}

JNIEXPORT jint JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeStartTransmit(
    JNIEnv *env, jobject /*thiz*/, jlong ptr, jstring channelId) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    const char *chId = env->GetStringUTFChars(channelId, nullptr);
    int result = ae_engine_start_transmit(engine, chId);
    env->ReleaseStringUTFChars(channelId, chId);
    return result;
}

JNIEXPORT jint JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeStopTransmit(
    JNIEnv *env, jobject /*thiz*/, jlong ptr, jstring channelId) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    const char *chId = env->GetStringUTFChars(channelId, nullptr);
    int result = ae_engine_stop_transmit(engine, chId);
    env->ReleaseStringUTFChars(channelId, chId);
    return result;
}

JNIEXPORT jint JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeSetChannelVolume(
    JNIEnv *env, jobject /*thiz*/, jlong ptr, jstring channelId, jfloat volume) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    const char *chId = env->GetStringUTFChars(channelId, nullptr);
    int result = ae_engine_set_channel_volume(engine, chId, volume);
    env->ReleaseStringUTFChars(channelId, chId);
    return result;
}

JNIEXPORT void JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeSetMasterVolume(
    JNIEnv * /*env*/, jobject /*thiz*/, jlong ptr, jfloat volume) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    ae_engine_set_master_volume(engine, volume);
}

JNIEXPORT jint JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeSetChannelMuted(
    JNIEnv *env, jobject /*thiz*/, jlong ptr, jstring channelId, jboolean muted) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    const char *chId = env->GetStringUTFChars(channelId, nullptr);
    int result = ae_engine_set_channel_muted(engine, chId, muted == JNI_TRUE);
    env->ReleaseStringUTFChars(channelId, chId);
    return result;
}

JNIEXPORT jint JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeStartAudio(JNIEnv * /*env*/, jobject /*thiz*/, jlong ptr) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    g_audioIO = std::make_unique<OboeAudioIO>(engine);
    return g_audioIO->start();
}

JNIEXPORT void JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeStopAudio(JNIEnv * /*env*/, jobject /*thiz*/, jlong ptr) {
    if (g_audioIO) {
        g_audioIO->stop();
        g_audioIO.reset();
    }
}

JNIEXPORT jfloat JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeGetRtt(JNIEnv * /*env*/, jobject /*thiz*/, jlong ptr) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    return ae_engine_get_stats(engine).rtt_ms;
}

JNIEXPORT jfloat JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeGetPacketLoss(JNIEnv * /*env*/, jobject /*thiz*/, jlong ptr) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    return ae_engine_get_stats(engine).packet_loss_pct;
}

JNIEXPORT jint JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeGetBufferUnderruns(JNIEnv * /*env*/, jobject /*thiz*/, jlong ptr) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    return ae_engine_get_stats(engine).buffer_underruns;
}

JNIEXPORT jint JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeGetActiveStreams(JNIEnv * /*env*/, jobject /*thiz*/, jlong ptr) {
    auto *engine = reinterpret_cast<ae_engine_t *>(ptr);
    return ae_engine_get_stats(engine).active_streams;
}

JNIEXPORT void JNICALL
Java_com_audioplatform_engine_AudioEngineJNI_nativeSetEventListener(
    JNIEnv *env, jobject /*thiz*/, jlong ptr, jobject listener) {
    if (g_eventListener) {
        env->DeleteGlobalRef(g_eventListener);
    }
    g_eventListener = env->NewGlobalRef(listener);
}

} // extern "C"
