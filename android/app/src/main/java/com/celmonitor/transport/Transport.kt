package com.celmonitor.transport

import java.io.Closeable
import java.io.InputStream
import java.io.OutputStream

/** A connected byte stream to the Windows host. The protocol layer is the same for every transport. */
interface Connection : Closeable {
    /** Human-readable, e.g. "USB (ADB)" or "USB (Accessory)". */
    val description: String
    val input: InputStream
    val output: OutputStream
}

/** Produces connections. [accept] blocks until the host connects or [close] is called. */
interface ConnectionSource : Closeable {
    val name: String
    fun accept(): Connection?
}
