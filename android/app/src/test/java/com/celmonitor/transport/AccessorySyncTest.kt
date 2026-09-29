package com.celmonitor.transport

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException

class AccessorySyncTest {
    private fun marker(tag: String, nonce: Long) =
        tag.toByteArray(Charsets.US_ASCII) + ByteArray(8) { i -> (nonce ushr (8 * i)).toByte() }

    @Test fun skipsStaleDataAndStopsAfterMatchingDone() {
        val stream = ByteArrayOutputStream().apply {
            write(byteArrayOf(0x43, 0x45, 0x4C, 0x4D, 1, 2, 3))   // tail of an old message
            write(marker("CELMSYNC", 111))                         // stale request from an earlier attempt
            write(marker("CELMSDON", 999))                         // DONE for a nonce never answered: ignored
            write(marker("CELMSYNC", 222))                         // current request
            write(marker("CELMSYNC", 222))                         // repeated by the PC before it saw the ACK
            write(marker("CELMSYNC", 222))
            write(marker("CELMSDON", 222))                         // done -> protocol starts
            write(byteArrayOf(9, 9, 9))                            // first protocol bytes, must not be consumed
        }.toByteArray()
        val input = ByteArrayInputStream(stream)
        val out = ByteArrayOutputStream()
        AccessorySync.awaitHost(input, out)
        assertArrayEquals(marker("CELMSACK", 111) + marker("CELMSACK", 222), out.toByteArray())
        assertEquals(3, input.available())
    }

    @Test fun throwsWhenStreamEndsBeforeSync() {
        val input = ByteArrayInputStream(marker("CELMSYNC", 5))
        assertThrows(IOException::class.java) { AccessorySync.awaitHost(input, ByteArrayOutputStream()) }
    }
}
