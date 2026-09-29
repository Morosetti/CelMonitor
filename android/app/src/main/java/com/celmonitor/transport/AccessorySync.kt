package com.celmonitor.transport

import java.io.IOException
import java.io.InputStream
import java.io.OutputStream

/**
 * Stream synchronization for the USB accessory channel (see docs/PROTOCOLO.md, "Sincronização do USB acessório").
 * Mirror of Synchronize() in windows/host/src/transport/AoaTransport.cpp.
 *
 * The accessory channel outlives sessions, so bytes from a previous attempt may still be queued. The phone discards
 * everything until the PC's SYNC marker, answers each one with an ACK carrying the same nonce, and starts the protocol
 * only after the DONE marker for the last nonce it answered.
 */
object AccessorySync {
    private val REQ = "CELMSYNC".toByteArray(Charsets.US_ASCII)
    private val ACK = "CELMSACK".toByteArray(Charsets.US_ASCII)
    private val DONE = "CELMSDON".toByteArray(Charsets.US_ASCII)
    private const val MARKER = 16

    /** Blocks until synchronized. @throws IOException if the accessory goes away. */
    fun awaitHost(input: InputStream, output: OutputStream) {
        val window = ByteArray(MARKER)
        var filled = 0
        var answered: Long? = null
        while (true) {
            val b = input.read()
            if (b < 0) throw IOException("accessory closed during sync")
            if (filled < MARKER) window[filled++] = b.toByte()
            else { System.arraycopy(window, 1, window, 0, MARKER - 1); window[MARKER - 1] = b.toByte() }
            if (filled < MARKER) continue
            val nonce = nonceOf(window)
            when {
                tagIs(window, REQ) -> {
                    // The PC repeats its request until answered: reply once per nonce, or the extra ACKs would
                    // reach the PC after the sync as if they were protocol data.
                    if (nonce != answered) {
                        output.write(ACK + window.copyOfRange(8, MARKER))
                        output.flush()
                        answered = nonce
                    }
                    filled = 0
                }
                tagIs(window, DONE) && nonce == answered -> return
            }
        }
    }

    private fun tagIs(w: ByteArray, tag: ByteArray): Boolean {
        for (i in 0 until 8) if (w[i] != tag[i]) return false
        return true
    }

    private fun nonceOf(w: ByteArray): Long {
        var v = 0L
        for (i in 0 until 8) v = v or ((w[8 + i].toLong() and 0xFF) shl (8 * i))
        return v
    }
}
