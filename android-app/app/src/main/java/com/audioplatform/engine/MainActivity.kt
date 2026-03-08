package com.audioplatform.engine

import android.Manifest
import android.annotation.SuppressLint
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.MotionEvent
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.*

class MainActivity : AppCompatActivity(), AudioEventListener {

    private val jni = AudioEngineJNI()
    private var enginePtr: Long = 0
    private var isConnected = false
    private val joinedChannels = mutableSetOf<String>()
    private var activeTransmitChannel: String? = null

    private val channelIds = listOf("channel-1", "channel-2", "channel-3")
    private val channelNames = listOf("Operations", "Dispatch", "Emergency")

    // UI elements
    private lateinit var statusDot: android.view.View
    private lateinit var statusText: TextView
    private lateinit var statsText: TextView
    private lateinit var serverHostInput: EditText
    private lateinit var connectButton: com.google.android.material.button.MaterialButton
    private lateinit var pttButton: com.google.android.material.button.MaterialButton
    private lateinit var volumeSlider: SeekBar

    private data class ChannelViews(
        val card: android.view.View,
        val name: TextView,
        val users: TextView,
        val volume: SeekBar,
        val joinButton: com.google.android.material.button.MaterialButton
    )
    private lateinit var channelViews: List<ChannelViews>

    private val scope = CoroutineScope(Dispatchers.Main + SupervisorJob())
    private var statsJob: Job? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        requestAudioPermission()
        initUI()
        createEngine()
    }

    private fun requestAudioPermission() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this,
                arrayOf(Manifest.permission.RECORD_AUDIO), 1)
        }
    }

    private fun initUI() {
        statusDot = findViewById(R.id.statusDot)
        statusText = findViewById(R.id.statusText)
        statsText = findViewById(R.id.statsText)
        serverHostInput = findViewById(R.id.serverHostInput)
        connectButton = findViewById(R.id.connectButton)
        pttButton = findViewById(R.id.pttButton)
        volumeSlider = findViewById(R.id.volumeSlider)

        // Initialize channel card views
        val cardIds = listOf(R.id.channel1Card, R.id.channel2Card, R.id.channel3Card)
        channelViews = cardIds.mapIndexed { index, cardId ->
            val card = findViewById<android.view.View>(cardId)
            val views = ChannelViews(
                card = card,
                name = card.findViewById(R.id.channelName),
                users = card.findViewById(R.id.channelUsers),
                volume = card.findViewById(R.id.channelVolume),
                joinButton = card.findViewById(R.id.joinButton)
            )
            views.name.text = channelNames[index]
            views.joinButton.setOnClickListener { toggleChannel(index) }
            views.volume.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                    if (fromUser && enginePtr != 0L) {
                        jni.nativeSetChannelVolume(enginePtr, channelIds[index], progress / 100f)
                    }
                }
                override fun onStartTrackingTouch(seekBar: SeekBar?) {}
                override fun onStopTrackingTouch(seekBar: SeekBar?) {}
            })
            views
        }

        connectButton.setOnClickListener { toggleConnection() }
        setupPTTButton()

        volumeSlider.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                if (fromUser && enginePtr != 0L) {
                    jni.nativeSetMasterVolume(enginePtr, progress / 100f)
                }
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun setupPTTButton() {
        pttButton.setOnTouchListener { _, event ->
            when (event.action) {
                MotionEvent.ACTION_DOWN -> {
                    startTransmitting()
                    pttButton.text = "TRANSMITTING..."
                    pttButton.backgroundTintList = ContextCompat.getColorStateList(this, android.R.color.holo_green_dark)
                    true
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    stopTransmitting()
                    pttButton.text = "PUSH TO TALK"
                    pttButton.backgroundTintList = ContextCompat.getColorStateList(this, android.R.color.holo_red_dark)
                    true
                }
                else -> false
            }
        }
    }

    private fun createEngine() {
        enginePtr = jni.nativeCreate()
        if (enginePtr != 0L) {
            jni.nativeSetEventListener(enginePtr, this)
        }
    }

    private fun toggleConnection() {
        if (isConnected) {
            jni.nativeStopAudio(enginePtr)
            jni.nativeDisconnect(enginePtr)
            joinedChannels.clear()
            activeTransmitChannel = null
            updateUI()
        } else {
            val host = serverHostInput.text.toString().trim()
            if (host.isEmpty()) {
                Toast.makeText(this, "Enter server IP", Toast.LENGTH_SHORT).show()
                return
            }
            connectButton.isEnabled = false
            connectButton.text = "Connecting..."

            scope.launch(Dispatchers.IO) {
                val result = jni.nativeConnect(enginePtr, host, 10000, 8080, "poc-token")
                withContext(Dispatchers.Main) {
                    if (result == 0) {
                        jni.nativeStartAudio(enginePtr)
                        startStatsUpdate()
                    } else {
                        connectButton.isEnabled = true
                        connectButton.text = "Connect"
                        Toast.makeText(this@MainActivity, "Connection failed: $result", Toast.LENGTH_SHORT).show()
                    }
                }
            }
        }
    }

    private fun toggleChannel(index: Int) {
        if (enginePtr == 0L || !isConnected) return
        val channelId = channelIds[index]

        scope.launch(Dispatchers.IO) {
            if (joinedChannels.contains(channelId)) {
                jni.nativeLeaveChannel(enginePtr, channelId)
                withContext(Dispatchers.Main) {
                    joinedChannels.remove(channelId)
                    updateChannelUI(index)
                }
            } else {
                val result = jni.nativeJoinChannel(enginePtr, channelId)
                withContext(Dispatchers.Main) {
                    if (result == 0) {
                        joinedChannels.add(channelId)
                        updateChannelUI(index)
                    }
                }
            }
            withContext(Dispatchers.Main) {
                pttButton.isEnabled = joinedChannels.isNotEmpty()
            }
        }
    }

    private fun startTransmitting() {
        if (enginePtr == 0L || joinedChannels.isEmpty()) return
        // Transmit on the first joined channel (or could let user select)
        val channelId = joinedChannels.first()
        scope.launch(Dispatchers.IO) {
            jni.nativeStartTransmit(enginePtr, channelId)
            activeTransmitChannel = channelId
        }
    }

    private fun stopTransmitting() {
        if (enginePtr == 0L) return
        activeTransmitChannel?.let { channelId ->
            scope.launch(Dispatchers.IO) {
                jni.nativeStopTransmit(enginePtr, channelId)
                activeTransmitChannel = null
            }
        }
    }

    private fun startStatsUpdate() {
        statsJob?.cancel()
        statsJob = scope.launch {
            while (isActive) {
                if (enginePtr != 0L && isConnected) {
                    val rtt = jni.nativeGetRtt(enginePtr)
                    val loss = jni.nativeGetPacketLoss(enginePtr)
                    statsText.text = "RTT: %.0f ms | Loss: %.1f%%".format(rtt, loss)
                }
                delay(1000)
            }
        }
    }

    private fun updateUI() {
        runOnUiThread {
            if (isConnected) {
                statusDot.setBackgroundResource(R.drawable.status_dot_green)
                statusText.text = "Connected"
                connectButton.text = "Disconnect"
                connectButton.isEnabled = true
            } else {
                statusDot.setBackgroundResource(R.drawable.status_dot_red)
                statusText.text = "Disconnected"
                connectButton.text = "Connect"
                connectButton.isEnabled = true
                pttButton.isEnabled = false
                statsJob?.cancel()
                statsText.text = "RTT: -- ms"
            }
            channelViews.forEachIndexed { index, _ -> updateChannelUI(index) }
        }
    }

    private fun updateChannelUI(index: Int) {
        val views = channelViews[index]
        val isJoined = joinedChannels.contains(channelIds[index])
        views.joinButton.text = if (isJoined) "Leave" else "Join"
        views.card.alpha = if (isJoined) 1.0f else 0.5f
    }

    // ─── AudioEventListener callbacks (called from native via JNI) ───

    override fun onConnected() {
        isConnected = true
        updateUI()
    }

    override fun onDisconnected() {
        isConnected = false
        joinedChannels.clear()
        activeTransmitChannel = null
        updateUI()
    }

    override fun onUserJoined(channelId: String, clientId: String, userName: String) {
        runOnUiThread {
            Toast.makeText(this, "$userName joined $channelId", Toast.LENGTH_SHORT).show()
        }
    }

    override fun onUserLeft(channelId: String, clientId: String) {
        runOnUiThread {
            Toast.makeText(this, "User left $channelId", Toast.LENGTH_SHORT).show()
        }
    }

    override fun onUserSpeaking(channelId: String, clientId: String) {
        // Could add visual indicator
    }

    override fun onUserStopped(channelId: String, clientId: String) {
        // Could remove visual indicator
    }

    override fun onError(message: String) {
        runOnUiThread {
            Toast.makeText(this, "Error: $message", Toast.LENGTH_LONG).show()
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        scope.cancel()
        if (enginePtr != 0L) {
            jni.nativeStopAudio(enginePtr)
            jni.nativeDisconnect(enginePtr)
            jni.nativeDestroy(enginePtr)
            enginePtr = 0
        }
    }
}
