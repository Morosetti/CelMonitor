// CelMonitor wire protocol v1 — Kotlin mirror of common/protocol/celmon_protocol.h.
// Spec: docs/PROTOCOLO.md. Keep values identical to the C++ side.
package com.celmonitor.protocol

import java.nio.ByteBuffer
import java.nio.ByteOrder

object Proto {
    const val MAGIC = 0x4D4C4543 // "CELM"
    const val VERSION_MAJOR = 1
    const val VERSION_MINOR = 0
    const val HEADER_SIZE = 16
    const val MAX_VIDEO_PAYLOAD = 8 * 1024 * 1024
    const val MAX_CONTROL_PAYLOAD = 4 * 1024
    const val MAX_STRING = 256
    const val FLAG_IGNORABLE = 0x0001

    const val MIN_DIMENSION = 320
    const val MAX_DIMENSION = 7680
    const val MAX_FPS = 240
    const val MAX_MODES = 32
    const val MAX_TOUCH_CONTACTS = 10

    const val CAP_H264 = 1 shl 0
    const val CAP_H265 = 1 shl 1
    const val CAP_AV1 = 1 shl 2
    const val CAP_TOUCH = 1 shl 8
    const val CAP_LOW_LATENCY_DECODER = 1 shl 9

    const val FRAME_KEY = 1 shl 0
    const val FRAME_CODEC_CONFIG = 1 shl 1
    const val ACK_RENDERED = 1

    fun validDimension(v: Int) = v in MIN_DIMENSION..MAX_DIMENSION
    fun validFps(v: Int) = v in 1..MAX_FPS
}

object MsgType {
    const val HELLO = 0x0001
    const val HELLO_ACK = 0x0002
    const val STREAM_CONFIG = 0x0003
    const val STREAM_READY = 0x0004
    const val VIDEO_FRAME = 0x0010
    const val FRAME_ACK = 0x0011
    const val REQUEST_KEYFRAME = 0x0012
    const val HEARTBEAT = 0x0020
    const val CLIENT_SETTINGS = 0x0021
    const val HOST_STATUS = 0x0022
    const val INPUT_MOUSE = 0x0030
    const val INPUT_TOUCH = 0x0031
    const val NOTICE = 0x00F0
    const val DISCONNECT = 0x00FF

    private val known = setOf(
        HELLO, HELLO_ACK, STREAM_CONFIG, STREAM_READY, VIDEO_FRAME, FRAME_ACK, REQUEST_KEYFRAME,
        HEARTBEAT, CLIENT_SETTINGS, HOST_STATUS, INPUT_MOUSE, INPUT_TOUCH, NOTICE, DISCONNECT,
    )

    fun isKnown(t: Int) = t in known
    fun maxPayload(t: Int) = if (t == VIDEO_FRAME) Proto.MAX_VIDEO_PAYLOAD else Proto.MAX_CONTROL_PAYLOAD
}

enum class Codec(val wire: Int, val mime: String, val capBit: Int) {
    H264(1, "video/avc", Proto.CAP_H264),
    H265(2, "video/hevc", Proto.CAP_H265),
    AV1(3, "video/av01", Proto.CAP_AV1);

    /** Bit used in DisplayMode.codecMask. */
    val maskBit get() = 1 shl (wire - 1)

    companion object {
        fun fromWire(v: Int) = entries.firstOrNull { it.wire == v }
    }
}

object DisconnectReason {
    const val NORMAL = 0
    const val PROTOCOL_ERROR = 1
    const val VERSION_MISMATCH = 2
    const val TIMEOUT = 3
    const val DISPLAY_ERROR = 4
    const val ENCODER_ERROR = 5
    const val DECODER_ERROR = 6
    const val SHUTDOWN = 7
}

object KeyframeReason {
    const val DECODER_CREATED = 1
    const val DECODE_ERROR = 2
    const val SURFACE_RECREATED = 3
}

object MouseAction { const val MOVE = 1; const val DOWN = 2; const val UP = 3; const val WHEEL = 4 }
object MouseButton { const val NONE = 0; const val LEFT = 1; const val RIGHT = 2; const val MIDDLE = 3 }
object TouchAction { const val DOWN = 1; const val MOVE = 2; const val UP = 3; const val CANCEL = 4 }

class ProtocolException(message: String) : Exception(message)

// ------------------------------------------------------------------ messages

data class DisplayMode(val width: Int, val height: Int, val maxFps: Int, val codecMask: Int)

data class Hello(
    val versionMajor: Int = Proto.VERSION_MAJOR,
    val versionMinor: Int = Proto.VERSION_MINOR,
    val capabilities: Int,
    val screenWidth: Int,
    val screenHeight: Int,
    val densityDpi: Int,
    val refreshRateMilliHz: Int,
    val rotation: Int,
    val modes: List<DisplayMode>,
    val deviceId: String,
    val manufacturer: String,
    val model: String,
    val androidVersion: String,
    val appVersion: String,
)

data class HelloAck(val versionMajor: Int, val versionMinor: Int, val hostCapabilities: Int, val sessionId: Int, val hostName: String)

data class StreamConfig(
    val streamId: Int, val width: Int, val height: Int, val fps: Int,
    val codec: Codec, val orientation: Int, val bitrateKbps: Int, val flags: Int,
)

data class VideoFrameHeader(val streamId: Int, val frameNumber: Int, val captureTimeUs: Long, val ptsUs: Long, val flags: Int) {
    val isKey get() = flags and Proto.FRAME_KEY != 0
    val isCodecConfig get() = flags and Proto.FRAME_CODEC_CONFIG != 0

    companion object { const val SIZE = 28 }
}

data class FrameAck(val streamId: Int, val frameNumber: Int, val captureTimeUs: Long, val clientProcessingUs: Int, val flags: Int)
data class Heartbeat(val senderTimeUs: Long, val echoTimeUs: Long, val counter: Int)
data class ClientSettings(val mask: Int, val fps: Int, val quality: Int, val orientation: Int) {
    companion object { const val MASK_FPS = 1; const val MASK_QUALITY = 2; const val MASK_ORIENTATION = 4 }
}
data class HostStatus(val encodeFpsX100: Int, val bitrateKbps: Int, val latencyUs: Int, val flags: Int, val encoderName: String)
data class InputMouse(val action: Int, val button: Int, val x: Int, val y: Int, val wheelV: Int = 0, val wheelH: Int = 0)
data class TouchContact(val id: Int, val action: Int, val x: Int, val y: Int)
data class Notice(val code: Int, val severity: Int, val message: String)
data class Disconnect(val reason: Int, val message: String)

// ------------------------------------------------------------------ byte I/O

class Writer(capacity: Int = 64) {
    private var buf = ByteBuffer.allocate(capacity).order(ByteOrder.LITTLE_ENDIAN)

    private fun ensure(n: Int) {
        if (buf.remaining() >= n) return
        val bigger = ByteBuffer.allocate(maxOf(buf.capacity() * 2, buf.position() + n)).order(ByteOrder.LITTLE_ENDIAN)
        buf.flip(); bigger.put(buf); buf = bigger
    }

    fun u8(v: Int) = apply { ensure(1); buf.put(v.toByte()) }
    fun u16(v: Int) = apply { ensure(2); buf.putShort(v.toShort()) }
    fun u32(v: Int) = apply { ensure(4); buf.putInt(v) }
    fun u64(v: Long) = apply { ensure(8); buf.putLong(v) }
    fun str(s: String) = apply {
        var b = s.toByteArray(Charsets.UTF_8)
        if (b.size > Proto.MAX_STRING) b = b.copyOf(Proto.MAX_STRING)
        u16(b.size); ensure(b.size); buf.put(b)
    }

    fun toByteArray(): ByteArray = buf.array().copyOf(buf.position())
}

/** Bounds-checked reader; any overrun throws ProtocolException. Trailing bytes are allowed. */
class Reader(data: ByteArray, offset: Int = 0, length: Int = data.size - offset) {
    private val buf = ByteBuffer.wrap(data, offset, length).order(ByteOrder.LITTLE_ENDIAN)

    val remaining get() = buf.remaining()
    val position get() = buf.position()

    private fun need(n: Int) { if (buf.remaining() < n) throw ProtocolException("truncated payload") }
    fun u8(): Int { need(1); return buf.get().toInt() and 0xFF }
    fun u16(): Int { need(2); return buf.getShort().toInt() and 0xFFFF }
    fun i16(): Int { need(2); return buf.getShort().toInt() }
    fun u32(): Int { need(4); return buf.getInt() }
    fun u64(): Long { need(8); return buf.getLong() }
    fun str(): String {
        val n = u16()
        if (n > Proto.MAX_STRING) throw ProtocolException("string too long")
        need(n)
        val b = ByteArray(n); buf.get(b)
        return String(b, Charsets.UTF_8)
    }
}

// ------------------------------------------------------------------ codec

object Codecs {
    fun encode(m: Hello): ByteArray = Writer(256).apply {
        u16(m.versionMajor); u16(m.versionMinor); u32(m.capabilities)
        u32(m.screenWidth); u32(m.screenHeight); u32(m.densityDpi); u32(m.refreshRateMilliHz); u32(m.rotation)
        val modes = m.modes.take(Proto.MAX_MODES)
        u16(modes.size); u16(0); u32(0)
        for (d in modes) { u32(d.width); u32(d.height); u16(d.maxFps); u16(d.codecMask) }
        str(m.deviceId); str(m.manufacturer); str(m.model); str(m.androidVersion); str(m.appVersion)
    }.toByteArray()

    fun encode(m: HelloAck): ByteArray = Writer().apply {
        u16(m.versionMajor); u16(m.versionMinor); u32(m.hostCapabilities); u32(m.sessionId); str(m.hostName)
    }.toByteArray()

    fun encode(m: StreamConfig): ByteArray = Writer().apply {
        u32(m.streamId); u32(m.width); u32(m.height); u16(m.fps); u8(m.codec.wire); u8(m.orientation)
        u32(m.bitrateKbps); u32(m.flags)
    }.toByteArray()

    fun encodeStreamReady(streamId: Int, status: Int): ByteArray = Writer().u32(streamId).u32(status).toByteArray()

    fun encode(m: FrameAck): ByteArray = Writer().apply {
        u32(m.streamId); u32(m.frameNumber); u64(m.captureTimeUs); u32(m.clientProcessingUs); u32(m.flags)
    }.toByteArray()

    fun encodeRequestKeyframe(streamId: Int, reason: Int): ByteArray = Writer().u32(streamId).u32(reason).toByteArray()

    fun encode(m: Heartbeat): ByteArray = Writer().apply {
        u64(m.senderTimeUs); u64(m.echoTimeUs); u32(m.counter); u32(0)
    }.toByteArray()

    fun encode(m: ClientSettings): ByteArray = Writer().apply {
        u32(m.mask); u16(m.fps); u8(m.quality); u8(m.orientation); u32(0)
    }.toByteArray()

    fun encode(m: InputMouse): ByteArray = Writer().apply {
        u8(m.action); u8(m.button); u16(0); u16(m.x); u16(m.y); u16(m.wheelV); u16(m.wheelH)
    }.toByteArray()

    fun encodeTouch(contacts: List<TouchContact>): ByteArray = Writer().apply {
        val c = contacts.take(Proto.MAX_TOUCH_CONTACTS)
        u8(c.size); u8(0); u8(0); u8(0)
        for (t in c) { u8(t.id); u8(t.action); u16(0); u16(t.x); u16(t.y) }
    }.toByteArray()

    fun encode(m: Disconnect): ByteArray = Writer().apply { u16(m.reason); u16(0); str(m.message) }.toByteArray()

    fun encode(m: HostStatus): ByteArray = Writer().apply {
        u32(m.encodeFpsX100); u32(m.bitrateKbps); u32(m.latencyUs); u32(m.flags); u32(0); str(m.encoderName)
    }.toByteArray()

    fun encode(m: Notice): ByteArray = Writer().apply { u16(m.code); u16(m.severity); str(m.message) }.toByteArray()

    // ---- parsers: validate structure and ranges, throw ProtocolException on anything invalid.

    fun parseHello(b: ByteArray): Hello {
        val r = Reader(b)
        val major = r.u16(); val minor = r.u16(); val caps = r.u32()
        val sw = r.u32(); val sh = r.u32(); val dpi = r.u32(); val refresh = r.u32(); val rot = r.u32()
        val count = r.u16(); r.u16(); r.u32()
        if (count !in 1..Proto.MAX_MODES || rot !in 0..3) throw ProtocolException("bad hello header")
        if (!Proto.validDimension(sw) || !Proto.validDimension(sh)) throw ProtocolException("bad screen size")
        val modes = List(count) {
            val d = DisplayMode(r.u32(), r.u32(), r.u16(), r.u16())
            if (!Proto.validDimension(d.width) || !Proto.validDimension(d.height) || !Proto.validFps(d.maxFps) || d.codecMask == 0)
                throw ProtocolException("bad mode")
            d
        }
        val h = Hello(major, minor, caps, sw, sh, dpi, refresh, rot, modes, r.str(), r.str(), r.str(), r.str(), r.str())
        if (h.deviceId.isEmpty()) throw ProtocolException("empty device id")
        return h
    }

    fun parseHelloAck(b: ByteArray): HelloAck {
        val r = Reader(b)
        return HelloAck(r.u16(), r.u16(), r.u32(), r.u32(), r.str())
    }

    fun parseStreamConfig(b: ByteArray): StreamConfig {
        val r = Reader(b)
        val id = r.u32(); val w = r.u32(); val h = r.u32(); val fps = r.u16()
        val codec = Codec.fromWire(r.u8()) ?: throw ProtocolException("unknown codec")
        val orientation = r.u8(); val bitrate = r.u32(); val flags = r.u32()
        if (!Proto.validDimension(w) || !Proto.validDimension(h) || !Proto.validFps(fps) || orientation !in 0..1)
            throw ProtocolException("bad stream config")
        return StreamConfig(id, w, h, fps, codec, orientation, bitrate, flags)
    }

    /** Parses the fixed part; the bitstream is payload[SIZE until end]. */
    fun parseVideoFrameHeader(b: ByteArray, length: Int): VideoFrameHeader {
        val r = Reader(b, 0, length)
        val h = VideoFrameHeader(r.u32(), r.u32(), r.u64(), r.u64(), r.u16())
        r.u16()
        if (r.remaining == 0) throw ProtocolException("empty video frame")
        return h
    }

    fun parseFrameAck(b: ByteArray): FrameAck {
        val r = Reader(b)
        return FrameAck(r.u32(), r.u32(), r.u64(), r.u32(), r.u32())
    }

    fun parseHeartbeat(b: ByteArray): Heartbeat {
        val r = Reader(b)
        return Heartbeat(r.u64(), r.u64(), r.u32())
    }

    fun parseHostStatus(b: ByteArray): HostStatus {
        val r = Reader(b)
        val fps = r.u32(); val kbps = r.u32(); val lat = r.u32(); val flags = r.u32(); r.u32()
        return HostStatus(fps, kbps, lat, flags, r.str())
    }

    fun parseNotice(b: ByteArray): Notice {
        val r = Reader(b)
        val n = Notice(r.u16(), r.u16(), r.str())
        if (n.severity !in 0..2) throw ProtocolException("bad severity")
        return n
    }

    fun parseDisconnect(b: ByteArray): Disconnect {
        val r = Reader(b)
        val reason = r.u16(); r.u16()
        return Disconnect(reason, r.str())
    }
}
