package com.celmonitor.protocol

import java.io.EOFException
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** A received message. [payload] may be larger than [length] (buffer reuse is not done; kept simple). */
class Message(val type: Int, val flags: Int, val seq: Int, val payload: ByteArray, val length: Int)

/**
 * Framing over a reliable byte stream (ADB socket or USB accessory).
 * Reads are single-threaded (one reader thread); writes are serialized by a lock.
 */
class MessageChannel(private val input: InputStream, private val output: OutputStream) {
    private val header = ByteArray(Proto.HEADER_SIZE)
    private val writeLock = Any()
    private var sendSeq = 0

    /** Blocks until a complete, validated message arrives. Unknown IGNORABLE messages are skipped. */
    fun read(): Message {
        while (true) {
            readFully(header, Proto.HEADER_SIZE)
            val bb = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN)
            val magic = bb.int
            val type = bb.short.toInt() and 0xFFFF
            val flags = bb.short.toInt() and 0xFFFF
            val length = bb.int
            val seq = bb.int
            if (magic != Proto.MAGIC) throw ProtocolException("bad magic")
            val known = MsgType.isKnown(type)
            val limit = if (known) MsgType.maxPayload(type) else Proto.MAX_CONTROL_PAYLOAD
            if (length < 0 || length > limit) throw ProtocolException("payload too large ($length) for type $type")
            val payload = ByteArray(length)
            readFully(payload, length)
            if (!known) {
                if (flags and Proto.FLAG_IGNORABLE != 0) continue
                throw ProtocolException("unknown message type $type")
            }
            return Message(type, flags, seq, payload, length)
        }
    }

    fun send(type: Int, payload: ByteArray, flags: Int = 0) {
        val hdr = ByteBuffer.allocate(Proto.HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        synchronized(writeLock) {
            hdr.putInt(Proto.MAGIC).putShort(type.toShort()).putShort(flags.toShort())
                .putInt(payload.size).putInt(sendSeq++)
            // One write per message keeps USB transfers large and avoids splitting headers from payloads.
            val out = ByteArray(Proto.HEADER_SIZE + payload.size)
            System.arraycopy(hdr.array(), 0, out, 0, Proto.HEADER_SIZE)
            System.arraycopy(payload, 0, out, Proto.HEADER_SIZE, payload.size)
            output.write(out)
            output.flush()
        }
    }

    private fun readFully(buf: ByteArray, n: Int) {
        var off = 0
        while (off < n) {
            val r = input.read(buf, off, n - off)
            if (r < 0) throw EOFException("connection closed")
            off += r
        }
    }
}
