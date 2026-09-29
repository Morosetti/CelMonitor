package com.celmonitor.transport

import android.net.LocalServerSocket
import android.net.LocalSocket
import android.os.Process
import android.util.Log
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream

/**
 * ADB transport: the host runs `adb forward tcp:N localabstract:celmonitor` and connects to N.
 * adbd on the phone then connects to our abstract socket. Everything travels over the USB cable.
 *
 * Only peers running as the adb shell user (uid 2000) or root are accepted, so other apps on the
 * phone cannot connect to the socket and impersonate the PC.
 */
class AdbSocketSource : ConnectionSource {
    override val name = "USB (ADB)"
    @Volatile private var server: LocalServerSocket? = null
    @Volatile private var closed = false

    override fun accept(): Connection? {
        while (!closed) {
            val srv = server ?: try {
                LocalServerSocket(SOCKET_NAME).also { server = it }
            } catch (e: IOException) {
                Log.e(TAG, "cannot bind abstract socket $SOCKET_NAME", e)
                return null
            }
            val sock = try { srv.accept() } catch (e: IOException) {
                if (closed) return null
                Log.w(TAG, "accept failed", e); continue
            }
            val uid = try { sock.peerCredentials.uid } catch (e: IOException) { -1 }
            if (uid != SHELL_UID && uid != Process.ROOT_UID) {
                Log.w(TAG, "rejected connection from uid $uid")
                runCatching { sock.close() }
                continue
            }
            return SocketConnection(sock)
        }
        return null
    }

    override fun close() {
        closed = true
        // LocalServerSocket.close() does not wake accept() on all versions; connecting to it does.
        runCatching { LocalSocket().use { it.connect(android.net.LocalSocketAddress(SOCKET_NAME)) } }
        runCatching { server?.close() }
        server = null
    }

    private class SocketConnection(private val sock: LocalSocket) : Connection {
        override val description = "USB (ADB)"
        override val input: InputStream = BufferedInputStream(sock.inputStream, 64 * 1024)
        override val output: OutputStream = BufferedOutputStream(sock.outputStream, 64 * 1024)
        override fun close() {
            runCatching { sock.shutdownInput() }
            runCatching { sock.shutdownOutput() }
            runCatching { sock.close() }
        }
    }

    companion object {
        const val SOCKET_NAME = "celmonitor"
        private const val SHELL_UID = 2000
        private const val TAG = "AdbSocketSource"
    }
}
