package com.celmonitor.transport

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.ParcelFileDescriptor
import android.util.Log
import java.io.BufferedInputStream
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream

/**
 * USB via Android Open Accessory: the PC switches the phone into accessory mode and talks over bulk endpoints,
 * without adb in the data path. When the PC starts accessory mode the system opens this app automatically
 * (res/xml/accessory_filter.xml); if the app was opened another way, permission is requested once.
 */
class AccessorySource(private val context: Context) : ConnectionSource {
    override val name = "USB (acessório)"
    private val usb = context.getSystemService(UsbManager::class.java)
    private val lock = Object()
    @Volatile private var closed = false
    @Volatile private var pending: Connection? = null
    private var permissionRequestedFor: UsbAccessory? = null

    // Last USB state reported by the system (sticky ACTION_USB_STATE broadcast).
    @Volatile private var accessoryOnline = false
    private val stateReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, intent: Intent) {
            accessoryOnline = intent.getBooleanExtra("connected", false) && intent.getBooleanExtra("configured", false) &&
                intent.getBooleanExtra("accessory", false)
            synchronized(lock) { lock.notifyAll() }
        }
    }

    init {
        // Hidden but stable since Android 4: "android.hardware.usb.action.USB_STATE" (sticky, system-sent).
        val filter = IntentFilter("android.hardware.usb.action.USB_STATE")
        if (Build.VERSION.SDK_INT >= 33) context.registerReceiver(stateReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        else context.registerReceiver(stateReceiver, filter)
    }

    override fun accept(): Connection? {
        while (!closed) {
            // Only open while the accessory function is really online. A read started while it is offline (e.g. right
            // after the cable is pulled, when the accessory is still listed) blocks in the kernel forever and keeps
            // /dev/usb_accessory busy — then Android ignores the next "start accessory" request from the PC.
            val acc = if (accessoryOnline) usb.accessoryList?.firstOrNull { it.manufacturer == MANUFACTURER && it.model == MODEL } else null
            if (acc == null) {
                permissionRequestedFor = null
                waitABit()
                continue
            }
            if (!usb.hasPermission(acc)) {
                if (permissionRequestedFor != acc) {
                    permissionRequestedFor = acc
                    requestPermission(acc)
                }
                waitABit()
                continue
            }
            val pfd = try { usb.openAccessory(acc) } catch (e: Exception) { Log.w(TAG, "openAccessory failed", e); null }
            if (pfd == null) { waitABit(); continue }
            Log.i(TAG, "accessory opened: ${acc.description}; waiting for the PC")
            val conn = AccessoryConnection(pfd)
            pending = conn
            try {
                AccessorySync.awaitHost(conn.input, conn.output)
            } catch (e: IOException) {
                Log.w(TAG, "accessory lost before sync: ${e.message}")
                conn.close()
                pending = null
                waitABit()
                continue
            }
            pending = null
            if (closed) { conn.close(); return null }  // monitor mode was left while waiting for the PC
            Log.i(TAG, "accessory synchronized with the PC")
            return conn
        }
        return null
    }

    private fun requestPermission(acc: UsbAccessory) {
        // Result is observed by polling hasPermission(); no receiver needed. Explicit package + mutable flag are
        // required on Android 12+/14 for the system to fill in the result extras.
        val intent = Intent(ACTION_PERMISSION).setPackage(context.packageName)
        val flags = if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
        usb.requestPermission(acc, PendingIntent.getBroadcast(context, 0, intent, flags))
    }

    private fun waitABit() = synchronized(lock) { if (!closed) lock.wait(1000) }

    override fun close() {
        closed = true
        runCatching { context.unregisterReceiver(stateReceiver) }
        pending?.close()  // unblocks a read waiting for the PC's sync marker
        synchronized(lock) { lock.notifyAll() }
    }

    private class AccessoryConnection(private val pfd: ParcelFileDescriptor) : Connection {
        override val description = "USB direto (AOA)"
        // The accessory driver (f_accessory) is picky about read sizes: requests smaller than the incoming transfer
        // can lose data, and on many kernels (e.g. Mi Max 3, Android 10) requests larger than its 16 KB bulk buffer fail
        // with EINVAL. So every read reaching the kernel is exactly 16 KB: BufferedInputStream always asks for its
        // buffer size, and CappedInputStream stops it from passing larger caller reads straight through.
        override val input: InputStream = BufferedInputStream(CappedInputStream(FileInputStream(pfd.fileDescriptor)), KERNEL_READ)
        override val output: OutputStream = FileOutputStream(pfd.fileDescriptor)
        override fun close() {
            runCatching { pfd.close() }
        }
    }

    private class CappedInputStream(input: InputStream) : java.io.FilterInputStream(input) {
        override fun read(b: ByteArray, off: Int, len: Int): Int = super.read(b, off, minOf(len, KERNEL_READ))

        // FileInputStream.available() is ioctl(FIONREAD), which f_accessory does not implement: it fails with EINVAL.
        // BufferedInputStream calls it whenever a large read is served in parts (i.e. on the first video frames),
        // which killed every accessory session ~2 s after connecting. Reporting 0 makes it return what it has.
        override fun available(): Int = 0
    }

    companion object {
        private const val KERNEL_READ = 16 * 1024  // f_accessory BULK_BUFFER_SIZE
        const val MANUFACTURER = "CelMonitor"   // must match windows/host/src/transport/AoaTransport.cpp
        const val MODEL = "CelMonitor Display"
        private const val ACTION_PERMISSION = "com.celmonitor.USB_ACCESSORY_PERMISSION"
        private const val TAG = "AccessorySource"
    }
}
