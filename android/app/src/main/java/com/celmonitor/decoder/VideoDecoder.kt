package com.celmonitor.decoder

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import com.celmonitor.display.DeviceDisplay
import com.celmonitor.protocol.StreamConfig
import com.celmonitor.protocol.VideoFrameHeader
import java.util.ArrayDeque

/**
 * Hardware decoder rendering straight into the SurfaceView's Surface (zero copy).
 * Frames are pushed from the network thread with [submit]; MediaCodec callbacks run on a private thread.
 */
class VideoDecoder(
    private val config: StreamConfig,
    private val surface: Surface,
    private val listener: Listener,
) {
    interface Listener {
        /** Frame handed to the decoder (flow-control ACK). */
        fun onFrameQueued(frame: VideoFrameHeader, processingUs: Int)
        /** Frame released to the display (latency ACK). */
        fun onFrameRendered(frame: VideoFrameHeader, processingUs: Int)
        fun onDecoderError(message: String)
    }

    private class Pending(val header: VideoFrameHeader, val data: ByteArray, val offset: Int, val length: Int, val receivedNs: Long)
    private class InFlight(val header: VideoFrameHeader, val receivedNs: Long)

    private val thread = HandlerThread("decoder", android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY).apply { start() }
    private val handler = Handler(thread.looper)
    private val lock = Any()
    private val pending = ArrayDeque<Pending>()
    private val freeInputs = ArrayDeque<Int>()
    private val inFlight = HashMap<Long, InFlight>()
    private var codec: MediaCodec? = null
    private var released = false
    private var waitingForKeyframe = true
    private var lastCodecConfig: Pending? = null

    var name: String = "?"; private set
    var lowLatencyEnabled = false; private set

    /** @throws Exception if no decoder can be configured for this stream. */
    fun start() {
        val info = DeviceDisplay.findDecoder(config.codec) ?: throw IllegalStateException("no ${config.codec} decoder")
        name = info.name
        val c = MediaCodec.createByCodecName(info.name)
        c.setCallback(callback, handler)
        try {
            c.configure(buildFormat(info, lowLatency = true), surface, null, 0)
            lowLatencyEnabled = true
        } catch (e: Exception) {
            // Some decoders reject vendor keys; fall back to a plain configuration.
            Log.w(TAG, "low-latency configure failed on ${info.name}, retrying plain", e)
            c.reset()
            c.setCallback(callback, handler)
            c.configure(buildFormat(info, lowLatency = false), surface, null, 0)
        }
        c.setVideoScalingMode(MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT)
        c.start()
        synchronized(lock) { codec = c }
        Log.i(TAG, "decoder ${info.name} started ${config.width}x${config.height}@${config.fps} lowLatency=$lowLatencyEnabled")
    }

    private fun buildFormat(info: MediaCodecInfo, lowLatency: Boolean): MediaFormat {
        val f = MediaFormat.createVideoFormat(config.codec.mime, config.width, config.height)
        f.setInteger(MediaFormat.KEY_PRIORITY, 0) // realtime
        f.setInteger(MediaFormat.KEY_OPERATING_RATE, config.fps)
        if (!lowLatency) return f
        if (Build.VERSION.SDK_INT >= 30 &&
            info.getCapabilitiesForType(config.codec.mime).isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)
        ) f.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
        val n = info.name.lowercase()
        when {
            n.contains("qcom") || n.contains("qti") -> {
                f.setInteger("vendor.qti-ext-dec-picture-order.enable", 1)
                f.setInteger("vendor.qti-ext-dec-low-latency.enable", 1)
            }
            n.contains("exynos") -> f.setInteger("vendor.rtc-ext-dec-low-latency.enable", 1)
            n.contains("hisi") -> {
                f.setInteger("vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-req", 1)
                f.setInteger("vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-rdy", -1)
            }
            n.contains("amlogic") -> f.setInteger("vendor.low-latency.enable", 1)
        }
        return f
    }

    /** Called from the network thread. [data] must not be reused by the caller. */
    fun submit(header: VideoFrameHeader, data: ByteArray, offset: Int, length: Int) {
        val p = Pending(header, data, offset, length, SystemClock.elapsedRealtimeNanos())
        synchronized(lock) {
            if (released) return
            if (header.isCodecConfig) {
                lastCodecConfig = p
            } else if (waitingForKeyframe) {
                if (!header.isKey) {
                    // Cannot decode P-frames without a reference; ACK so the host keeps flowing.
                    listener.onFrameQueued(header, 0)
                    return
                }
                waitingForKeyframe = false
            }
            pending.addLast(p)
            drainLocked()
        }
    }

    private fun drainLocked() {
        val c = codec ?: return
        while (pending.isNotEmpty() && freeInputs.isNotEmpty()) {
            val p = pending.removeFirst()
            val index = freeInputs.removeFirst()
            try {
                val buf = c.getInputBuffer(index) ?: continue
                if (buf.capacity() < p.length) {
                    listener.onDecoderError("frame of ${p.length} bytes exceeds decoder buffer ${buf.capacity()}")
                    return
                }
                buf.clear()
                buf.put(p.data, p.offset, p.length)
                val flags = if (p.header.isCodecConfig) MediaCodec.BUFFER_FLAG_CODEC_CONFIG
                else if (p.header.isKey) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0
                c.queueInputBuffer(index, 0, p.length, p.header.ptsUs, flags)
                if (!p.header.isCodecConfig) inFlight[p.header.ptsUs] = InFlight(p.header, p.receivedNs)
                listener.onFrameQueued(p.header, elapsedUs(p.receivedNs))
            } catch (e: IllegalStateException) {
                listener.onDecoderError("queueInputBuffer: ${e.message}")
                return
            }
        }
        // Protect against a runaway backlog (should not happen with host flow control).
        if (inFlight.size > 64) inFlight.clear()
    }

    /** Resends the cached SPS/PPS and waits for the next keyframe (e.g. after the host restarts the GOP). */
    fun resetToKeyframe() = synchronized(lock) {
        waitingForKeyframe = true
        pending.clear()
        lastCodecConfig?.let { pending.addLast(it); drainLocked() }
    }

    private val callback = object : MediaCodec.Callback() {
        override fun onInputBufferAvailable(c: MediaCodec, index: Int) {
            synchronized(lock) {
                if (released) return
                freeInputs.addLast(index)
                drainLocked()
            }
        }

        override fun onOutputBufferAvailable(c: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
            val f: InFlight?
            synchronized(lock) {
                if (released) return
                f = inFlight.remove(info.presentationTimeUs)
            }
            try {
                // Render immediately: the display latches it on the next vsync.
                c.releaseOutputBuffer(index, info.size > 0)
            } catch (e: IllegalStateException) {
                return
            }
            if (f != null) listener.onFrameRendered(f.header, elapsedUs(f.receivedNs))
        }

        override fun onError(c: MediaCodec, e: MediaCodec.CodecException) {
            Log.e(TAG, "codec error", e)
            listener.onDecoderError(e.diagnosticInfo ?: e.toString())
        }

        override fun onOutputFormatChanged(c: MediaCodec, format: MediaFormat) {
            Log.i(TAG, "output format $format")
        }
    }

    fun release() {
        val c: MediaCodec?
        synchronized(lock) {
            if (released) return
            released = true
            c = codec
            codec = null
            pending.clear(); freeInputs.clear(); inFlight.clear()
        }
        runCatching { c?.stop() }
        runCatching { c?.release() }
        thread.quitSafely()
    }

    private fun elapsedUs(sinceNs: Long) = ((SystemClock.elapsedRealtimeNanos() - sinceNs) / 1000).toInt()

    companion object { private const val TAG = "VideoDecoder" }
}
