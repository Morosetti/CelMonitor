package com.celmonitor.decoder

import android.view.Surface
import com.celmonitor.protocol.Codec
import com.celmonitor.protocol.StreamConfig
import com.celmonitor.protocol.VideoFrameHeader

/** Turns received frames into pictures on the SurfaceView's Surface. */
interface FrameDecoder {
    interface Listener {
        /** Frame accepted by the decoder (flow-control ACK). */
        fun onFrameQueued(frame: VideoFrameHeader, processingUs: Int)
        /** Frame released to the display (latency ACK). */
        fun onFrameRendered(frame: VideoFrameHeader, processingUs: Int)
        fun onDecoderError(message: String)
    }

    val name: String
    val lowLatencyEnabled: Boolean

    /** @throws Exception if the decoder cannot be set up for this stream. */
    fun start()

    /** Called from the network thread. [data] must not be reused by the caller. */
    fun submit(header: VideoFrameHeader, data: ByteArray, offset: Int, length: Int)

    /** Synchronous: nothing touches the Surface after this returns. */
    fun release()

    companion object {
        fun create(config: StreamConfig, surface: Surface, listener: Listener): FrameDecoder =
            if (config.codec == Codec.JPEG) JpegDecoder(config, surface, listener) else VideoDecoder(config, surface, listener)

        fun elapsedUs(sinceNs: Long) = ((System.nanoTime() - sinceNs) / 1000).toInt()
    }
}
