package com.audioplatform.engine

/**
 * JNI bridge to the shared C audio engine library.
 * All native methods map to functions in native-bridge.cpp which call the C core library.
 */
class AudioEngineJNI {

    companion object {
        init {
            System.loadLibrary("audioengine_android")
        }
    }

    // Engine lifecycle
    external fun nativeCreate(): Long  // Returns engine pointer
    external fun nativeDestroy(enginePtr: Long)

    // Connection
    external fun nativeConnect(enginePtr: Long, host: String, udpPort: Int, wsPort: Int, token: String): Int
    external fun nativeDisconnect(enginePtr: Long)
    external fun nativeIsConnected(enginePtr: Long): Boolean

    // Channels
    external fun nativeJoinChannel(enginePtr: Long, channelId: String): Int
    external fun nativeLeaveChannel(enginePtr: Long, channelId: String): Int
    external fun nativeLeaveAllChannels(enginePtr: Long)

    // Transmit
    external fun nativeStartTransmit(enginePtr: Long, channelId: String): Int
    external fun nativeStopTransmit(enginePtr: Long, channelId: String): Int

    // Volume
    external fun nativeSetChannelVolume(enginePtr: Long, channelId: String, volume: Float): Int
    external fun nativeSetMasterVolume(enginePtr: Long, volume: Float)
    external fun nativeSetChannelMuted(enginePtr: Long, channelId: String, muted: Boolean): Int

    // Audio I/O - Oboe handles this in native code
    external fun nativeStartAudio(enginePtr: Long): Int
    external fun nativeStopAudio(enginePtr: Long)

    // Stats
    external fun nativeGetRtt(enginePtr: Long): Float
    external fun nativeGetPacketLoss(enginePtr: Long): Float
    external fun nativeGetBufferUnderruns(enginePtr: Long): Int
    external fun nativeGetActiveStreams(enginePtr: Long): Int

    // Event callback registration
    external fun nativeSetEventListener(enginePtr: Long, listener: AudioEventListener)
}

/**
 * Callback interface for audio engine events.
 * Called from the native layer via JNI.
 */
interface AudioEventListener {
    fun onConnected()
    fun onDisconnected()
    fun onUserJoined(channelId: String, clientId: String, userName: String)
    fun onUserLeft(channelId: String, clientId: String)
    fun onUserSpeaking(channelId: String, clientId: String)
    fun onUserStopped(channelId: String, clientId: String)
    fun onError(message: String)
}
