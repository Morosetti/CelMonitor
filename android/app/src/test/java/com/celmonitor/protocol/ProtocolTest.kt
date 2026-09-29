package com.celmonitor.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.EOFException
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

class ProtocolTest {

    private val hello = Hello(
        capabilities = Proto.CAP_H264 or Proto.CAP_TOUCH,
        screenWidth = 1080, screenHeight = 2400, densityDpi = 420, refreshRateMilliHz = 60000, rotation = 0,
        modes = listOf(DisplayMode(2400, 1080, 60, 1), DisplayMode(1920, 1080, 60, 3)),
        deviceId = "abc123", manufacturer = "ACME", model = "Phone X", androidVersion = "14", appVersion = "0.1.0",
    )

    @Test fun helloRoundTrip() {
        assertEquals(hello, Codecs.parseHello(Codecs.encode(hello)))
    }

    @Test fun helloRejectsInvalidMode() {
        val bad = hello.copy(modes = listOf(DisplayMode(100, 100, 60, 1)))
        assertThrows(ProtocolException::class.java) { Codecs.parseHello(Codecs.encode(bad)) }
    }

    @Test fun helloRejectsTruncation() {
        val b = Codecs.encode(hello)
        assertThrows(ProtocolException::class.java) { Codecs.parseHello(b.copyOf(b.size - 3)) }
    }

    @Test fun trailingBytesAreIgnored() {
        val cfg = StreamConfig(7, 1920, 1080, 60, Codec.H264, 0, 12000, 0)
        val b = Codecs.encode(cfg) + byteArrayOf(1, 2, 3, 4)
        assertEquals(cfg, Codecs.parseStreamConfig(b))
    }

    @Test fun streamConfigRejectsBadCodecAndFps() {
        val b = Codecs.encode(StreamConfig(1, 1920, 1080, 60, Codec.H264, 0, 1, 0))
        b[14] = 9 // codec byte
        assertThrows(ProtocolException::class.java) { Codecs.parseStreamConfig(b) }
        val b2 = Codecs.encode(StreamConfig(1, 1920, 1080, 999, Codec.H264, 0, 1, 0))
        assertThrows(ProtocolException::class.java) { Codecs.parseStreamConfig(b2) }
    }

    @Test fun channelRoundTripAndIgnorableUnknown() {
        val out = ByteArrayOutputStream()
        val tx = MessageChannel(ByteArrayInputStream(ByteArray(0)), out)
        tx.send(0x7777, byteArrayOf(1, 2, 3), Proto.FLAG_IGNORABLE) // unknown, ignorable → skipped
        tx.send(MsgType.HEARTBEAT, Codecs.encode(Heartbeat(5, 6, 7)))
        val rx = MessageChannel(ByteArrayInputStream(out.toByteArray()), ByteArrayOutputStream())
        val m = rx.read()
        assertEquals(MsgType.HEARTBEAT, m.type)
        assertEquals(Heartbeat(5, 6, 7), Codecs.parseHeartbeat(m.payload))
        assertThrows(EOFException::class.java) { rx.read() }
    }

    @Test fun channelRejectsUnknownFatal() {
        val out = ByteArrayOutputStream()
        MessageChannel(ByteArrayInputStream(ByteArray(0)), out).send(0x7777, byteArrayOf(1))
        val rx = MessageChannel(ByteArrayInputStream(out.toByteArray()), ByteArrayOutputStream())
        assertThrows(ProtocolException::class.java) { rx.read() }
    }

    @Test fun channelRejectsBadMagicAndOversize() {
        val hdr = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
        hdr.putInt(0x12345678).putShort(MsgType.HEARTBEAT.toShort()).putShort(0).putInt(0).putInt(0)
        assertThrows(ProtocolException::class.java) {
            MessageChannel(ByteArrayInputStream(hdr.array()), ByteArrayOutputStream()).read()
        }
        val big = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
        big.putInt(Proto.MAGIC).putShort(MsgType.HEARTBEAT.toShort()).putShort(0).putInt(Proto.MAX_CONTROL_PAYLOAD + 1).putInt(0)
        assertThrows(ProtocolException::class.java) {
            MessageChannel(ByteArrayInputStream(big.array()), ByteArrayOutputStream()).read()
        }
    }

    @Test fun videoFrameHeader() {
        val w = Writer().u32(3).u32(42).u64(1000).u64(2000).u16(Proto.FRAME_KEY).u16(0).u8(0).u8(0).u8(1)
        val b = w.toByteArray()
        val h = Codecs.parseVideoFrameHeader(b, b.size)
        assertEquals(VideoFrameHeader(3, 42, 1000, 2000, Proto.FRAME_KEY), h)
        assertEquals(true, h.isKey)
    }

    /**
     * Cross-language check: vectors written by the C++ test (windows/tests) must decode to the same values,
     * and Kotlin must produce identical bytes. Skipped until the C++ side has generated them.
     */
    @Test fun crossLanguageVectors() {
        val dir = File("../../common/protocol/testvectors")
        val helloFile = File(dir, "hello.bin")
        org.junit.Assume.assumeTrue("C++ test vectors not generated yet", helloFile.exists())
        val cpp = helloFile.readBytes()
        assertEquals(hello, Codecs.parseHello(cpp))
        assertArrayEquals(cpp, Codecs.encode(hello))
        val cfg = File(dir, "stream_config.bin").readBytes()
        assertArrayEquals(cfg, Codecs.encode(StreamConfig(7, 1920, 1080, 60, Codec.H264, 0, 12000, 0)))
    }
}
