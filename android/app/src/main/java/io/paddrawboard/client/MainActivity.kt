package io.paddrawboard.client

import android.app.Activity
import android.os.Bundle
import android.view.View
import android.view.WindowInsets
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import io.paddrawboard.client.diagnostics.CapabilityProbe
import io.paddrawboard.client.input.InputCaptureView
import io.paddrawboard.client.protocol.*
import io.paddrawboard.client.transport.AdbSession
import io.paddrawboard.client.transport.SessionAuth
import io.paddrawboard.client.video.AvcDecoder
import io.paddrawboard.protocol.PdbProtocol

class MainActivity : Activity() {
    private val state = AppState()
    private lateinit var capture: InputCaptureView
    private lateinit var status: TextView
    private var session: AdbSession? = null
    private var decoder: AvcDecoder? = null
    @Volatile private var latestConfig: PdbProtocol.ServerConfig? = null
    @Volatile private var surfaceReady = false
    private var sessionToken: ByteArray? = null
    override fun onCreate(savedInstanceState: Bundle?) { super.onCreate(savedInstanceState); window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val root = FrameLayout(this); capture = InputCaptureView(this,
            { session?.sendInput(it, capture.width, capture.height) },
            { state.snapshot().palmRejectionEnabled },
            { bits -> session?.sendControl(PdbProtocol.CapabilityChanged(bits)) },
        ); root.addView(capture)
        status = TextView(this).apply { setBackgroundColor(0x99000000.toInt()); setTextColor(-1); setPadding(20, 20, 20, 20) }; root.addView(status); setContentView(root); requestImmersive()
        state.observe { runOnUiThread { status.text = "${it.connection}: ${it.status}\\n${it.capabilitySummary}" } }
        sessionToken = SessionAuth.parseTokenHex(intent.getStringExtra(SessionAuth.INTENT_EXTRA))
        if (sessionToken == null) {
            state.update { it.copy(connection = ConnectionState.ERROR, status = "Invalid or missing session authentication token") }
        }
        status.setOnClickListener {
            val capabilities = CapabilityProbe(this).inspect(capture.width, capture.height, rotationDegrees(), capture.penKeyCapabilities())
            val log = CapabilityProbe(this).export(capabilities)
            state.update { current -> current.copy(capabilitySummary = "${capabilities}\nExported: ${log.name}") }
        }
        capture.holder.addCallback(object : android.view.SurfaceHolder.Callback {
            override fun surfaceCreated(holder: android.view.SurfaceHolder) { surfaceReady = true }
            override fun surfaceChanged(holder: android.view.SurfaceHolder, format: Int, width: Int, height: Int) { if (session == null) start() else rotationChanged() }
            override fun surfaceDestroyed(holder: android.view.SurfaceHolder) { surfaceReady = false; stop() }
        })
    }
    private fun start() { if (session != null) return
        val token = sessionToken
        if (token == null) {
            state.update { it.copy(connection = ConnectionState.ERROR, status = "Invalid or missing session authentication token") }
            return
        }
        val probed = CapabilityProbe(this).inspect(capture.width, capture.height, rotationDegrees(), capture.penKeyCapabilities())
        val codec = PdbCodec()
        capture.initializeCapabilities(codec.capabilityBits(probed))
        val caps = codec.withCapabilityBits(probed, capture.currentCapabilityBits())
        if (!caps.h264Decoder) { state.update { it.copy(connection = ConnectionState.ERROR, status = "H.264 decoder unavailable", capabilitySummary = caps.toString()) }; return }
        state.update { it.copy(connection = ConnectionState.CONNECTING, capabilitySummary = caps.toString()) }
        session = AdbSession(object : AdbSession.Listener {
            override fun onControl(frame: PdbProtocol.Frame) { if (frame.payload is PdbProtocol.ServerConfig) {
                val config = frame.payload as PdbProtocol.ServerConfig
                latestConfig = config
                if (surfaceReady) configureDecoder(config)
            } }
            override fun onVideo(frame: PdbProtocol.Frame) { if (frame.payload is PdbProtocol.VideoFrame) { val video = frame.payload as PdbProtocol.VideoFrame; session?.recordVideoFrame(video.presentationTimestampNs); decoder?.queue(video.accessUnit, video.presentationTimestampNs / 1_000, frame.header.flags and PdbProtocol.VIDEO_FLAG_IDR != 0L) } }
            override fun onState(status: String, connected: Boolean) { state.update { it.copy(connection = if (connected) ConnectionState.CONNECTED else ConnectionState.RECONNECTING, status = status) } }
            override fun onFailure(error: Throwable) {
                cancelActivePointers()
                state.update { it.copy(connection = ConnectionState.ERROR, status = "Session error", lastError = error.message) }
            }
        }, token).also { it.start(caps) }
    }
    private fun cancelActivePointers() {
        capture.releasePointers().takeIf { it.isNotEmpty() }?.let { session?.sendInput(it, capture.width, capture.height) }
    }
    private fun configureDecoder(config: PdbProtocol.ServerConfig) {
        if (!surfaceReady) return
        val current = decoder
        if (current == null) {
            decoder = AvcDecoder(capture.holder.surface, { session?.sendControl(PdbProtocol.RequestIdr) }, { state.update { value -> value.copy(droppedVideoFrames = value.droppedVideoFrames + 1) } })
            decoder?.configure(config.videoWidth, config.videoHeight)
        } else {
            current.reconfigure(capture.holder.surface, config.videoWidth, config.videoHeight)
        }
    }
    private fun rotationChanged() {
        cancelActivePointers()
        session?.sendControl(PdbProtocol.OrientationChanged(rotationDegrees()))
        val config = latestConfig
        if (config != null && surfaceReady) {
            // Rebind to the current Surface and await a fresh IDR after rotation.
            decoder?.reconfigure(capture.holder.surface, config.videoWidth, config.videoHeight)
                ?: configureDecoder(config)
        } else {
            session?.sendControl(PdbProtocol.RequestIdr)
        }
    }
    private fun rotationDegrees(): Int = when (display?.rotation) { android.view.Surface.ROTATION_90 -> 90; android.view.Surface.ROTATION_180 -> 180; android.view.Surface.ROTATION_270 -> 270; else -> 0 }
    private fun stop() { cancelActivePointers(); decoder?.close(); decoder = null; latestConfig = null; session?.close(); session = null; state.update { it.copy(connection = ConnectionState.STOPPED, status = "Stopped") } }
    private fun requestImmersive() {
        val decor = window.decorView
        decor.post {
            if (isFinishing || isDestroyed || !decor.isAttachedToWindow) return@post
            val controller = decor.windowInsetsController ?: return@post
            controller.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
            controller.systemBarsBehavior = android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }
    override fun onWindowFocusChanged(hasFocus: Boolean) { super.onWindowFocusChanged(hasFocus); if (hasFocus) requestImmersive() }
    override fun onDestroy() { stop(); super.onDestroy() }
}
