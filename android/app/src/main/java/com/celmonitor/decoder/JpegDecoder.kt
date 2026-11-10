package com.celmonitor.decoder

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.util.Log
import android.view.Surface
import com.celmonitor.protocol.ProtocolException
import com.celmonitor.protocol.Reader
import com.celmonitor.protocol.StreamConfig
import com.celmonitor.protocol.VideoFrameHeader
import java.util.ArrayDeque
import java.util.concurrent.Callable
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * Software path for devices without a usable MediaCodec (Android 4.0): the PC sends JPEG tiles of the rows that
 * changed (docs/PROTOCOLO.md, "Codec JPEG"). Tiles are decoded with BitmapFactory, in parallel on every core, into a
 * copy of the whole screen, and the changed area is copied to the Surface with a software canvas.
 *
 * Updates build on each other, so none may be skipped: they are applied in order. The flow-control ACK is sent when
 * an update starts decoding; the PC (window of 1 update for JPEG) then captures the next one, so it arrives while this
 * one decodes and the device is never more than one update behind.
 */
class JpegDecoder(
    private val config: StreamConfig,
    private val surface: Surface,
    private val listener: FrameDecoder.Listener,
) : FrameDecoder {
    private class Update(val header: VideoFrameHeader, val data: ByteArray, val offset: Int, val length: Int, val receivedNs: Long)
    private class Tile(val x: Int, val y: Int, val w: Int, val h: Int, val offset: Int, val length: Int)

    override val name = "JPEG (CPU)"
    override val lowLatencyEnabled = false

    private val lock = Object()
    private val queue = ArrayDeque<Update>()
    @Volatile private var released = false
    private var waitingForKey = true
    private val thread = Thread(::loop, "celmon-jpeg")
    private val workers: ExecutorService =
        Executors.newFixedThreadPool(Runtime.getRuntime().availableProcessors().coerceIn(1, 4))

    // The whole picture, RGB_565: half the memory traffic of ARGB_8888 on a slow CPU, and what old devices' surfaces
    // use anyway. Tiles are decoded into reusable bitmaps (same size required for reuse before Android 4.4).
    private val screen = Bitmap.createBitmap(config.width, config.height, Bitmap.Config.RGB_565)
    private val screenCanvas = Canvas(screen)
    private val tileBitmaps = HashMap<Long, ArrayDeque<Bitmap>>()
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
    private val dirty = Rect()
    private val src = Rect()
    private val dst = Rect()
    private var failures = 0

    override fun start() {
        thread.priority = Thread.MAX_PRIORITY - 1
        thread.start()
        Log.i(TAG, "JPEG decoder started ${config.width}x${config.height}, ${Runtime.getRuntime().availableProcessors()} cores")
    }

    override fun submit(header: VideoFrameHeader, data: ByteArray, offset: Int, length: Int) {
        if (header.isCodecConfig) return
        synchronized(lock) {
            if (released) return
            if (header.isKey) queue.clear()  // a complete picture replaces everything before it
            queue.addLast(Update(header, data, offset, length, System.nanoTime()))
            lock.notifyAll()
        }
    }

    private fun loop() {
        try {
            while (true) {
                val u = synchronized(lock) {
                    while (queue.isEmpty() && !released) lock.wait()
                    if (released) return
                    queue.removeFirst()
                }
                listener.onFrameQueued(u.header, FrameDecoder.elapsedUs(u.receivedNs))
                if (waitingForKey && !u.header.isKey) continue  // nothing to apply this update to yet
                val t0 = System.nanoTime()
                if (!apply(u)) {
                    // A broken update leaves the copy of the screen wrong: start again from a complete picture.
                    waitingForKey = true
                    listener.onDecoderError("JPEG inválido")
                    continue
                }
                waitingForKey = false
                val t1 = System.nanoTime()
                if (!draw()) continue
                listener.onFrameRendered(u.header, FrameDecoder.elapsedUs(u.receivedNs))
                trace(u, t1 - t0, System.nanoTime() - t1)
            }
        } catch (_: InterruptedException) {
        }
    }

    /** Decodes every tile (in parallel) into [screen] and sets [dirty] to the area they cover. */
    private fun apply(u: Update): Boolean {
        val tiles = try { parseTiles(u) } catch (e: ProtocolException) {
            Log.w(TAG, "bad JPEG update: ${e.message}")
            return false
        }
        val decoded = if (tiles.size == 1) listOf(decodeTile(u, tiles[0]))
        else workers.invokeAll(tiles.map { t -> Callable { decodeTile(u, t) } }).map { it.get() }
        var ok = true
        dirty.setEmpty()
        for ((i, bmp) in decoded.withIndex()) {
            val t = tiles[i]
            if (bmp == null) { ok = false; continue }
            screenCanvas.drawBitmap(bmp, t.x.toFloat(), t.y.toFloat(), null)
            dirty.union(t.x, t.y, t.x + t.w, t.y + t.h)
            recycleTile(bmp)
        }
        if (!ok && ++failures % 30 == 1) Log.w(TAG, "undecodable JPEG tile, $failures so far")
        return ok
    }

    private fun parseTiles(u: Update): List<Tile> {
        val r = Reader(u.data, u.offset, u.length)
        val count = r.u16(); r.u16()
        if (count !in 1..MAX_TILES) throw ProtocolException("tile count $count")
        var pos = u.offset + 4 + 12 * count
        val tiles = List(count) {
            val t = Tile(r.u16(), r.u16(), r.u16(), r.u16(), 0, r.u32())
            if (t.w <= 0 || t.h <= 0 || t.x + t.w > config.width || t.y + t.h > config.height || t.length <= 0)
                throw ProtocolException("tile outside the picture")
            Tile(t.x, t.y, t.w, t.h, pos, t.length).also { pos += t.length }
        }
        if (pos > u.offset + u.length) throw ProtocolException("truncated tiles")
        return tiles
    }

    private fun decodeTile(u: Update, t: Tile): Bitmap? {
        val opts = BitmapFactory.Options().apply {
            inPreferredConfig = Bitmap.Config.RGB_565
            inMutable = true
            @Suppress("DEPRECATION") inDither = false
            inBitmap = takeTile(t.w, t.h)
        }
        val t0 = System.nanoTime()
        val bmp = try {
            BitmapFactory.decodeByteArray(u.data, t.offset, t.length, opts)
        } catch (e: IllegalArgumentException) {  // the reusable bitmap did not fit after all
            opts.inBitmap = null
            BitmapFactory.decodeByteArray(u.data, t.offset, t.length, opts)
        }
        val dt = System.nanoTime() - t0
        synchronized(tileBitmaps) { traceTiles++; traceTileNs += dt; if (dt > traceTileMaxNs) traceTileMaxNs = dt }
        return if (bmp != null && bmp.width == t.w && bmp.height == t.h) bmp else null
    }
    private var traceTiles = 0
    private var traceTileNs = 0L
    private var traceTileMaxNs = 0L

    private fun takeTile(w: Int, h: Int): Bitmap? = synchronized(tileBitmaps) {
        tileBitmaps[key(w, h)]?.pollFirst()
    }

    private fun recycleTile(b: Bitmap) = synchronized(tileBitmaps) {
        // Keep a few of each size: the same tile sizes come back again and again (full screens, typing on a line).
        val list = tileBitmaps.getOrPut(key(b.width, b.height)) { ArrayDeque() }
        if (list.size < 4 && tileBitmaps.size < 16) list.addLast(b)
    }

    private fun key(w: Int, h: Int) = (w.toLong() shl 32) or h.toLong()

    /** Copies the changed area of [screen] to the Surface. */
    private fun draw(): Boolean {
        if (released || !surface.isValid) return false
        // The system may enlarge the requested area (e.g. a new buffer whose content is unknown): it updates the
        // rect, and all of it must be drawn. Scaling (stream size != view size) redraws everything.
        val canvas = try {
            surface.lockCanvas(dirty)
        } catch (e: Exception) {  // IllegalArgumentException / Surface.OutOfResourcesException: surface going away
            return false
        } ?: return false
        try {
            if (canvas.width == screen.width && canvas.height == screen.height) {
                canvas.drawBitmap(screen, dirty, dirty, null)
            } else {
                if (!loggedScale) { loggedScale = true; Log.i(TAG, "scaling ${screen.width}x${screen.height} -> ${canvas.width}x${canvas.height}") }
                src.set(0, 0, screen.width, screen.height)
                dst.set(0, 0, canvas.width, canvas.height)
                canvas.drawBitmap(screen, src, dst, paint)
            }
        } finally {
            try { surface.unlockCanvasAndPost(canvas) } catch (_: Exception) {}
        }
        return true
    }
    private var loggedScale = false

    // Diagnostics, logged every 10 s while updates arrive.
    private var traceStartNs = 0L
    private var traceFrames = 0
    private var traceDecodeNs = 0L
    private var traceDrawNs = 0L
    private var traceBytes = 0L
    private var tracePixels = 0L

    private fun trace(u: Update, decodeNs: Long, drawNs: Long) {
        val now = System.nanoTime()
        if (traceFrames == 0) traceStartNs = now
        traceFrames++; traceDecodeNs += decodeNs; traceDrawNs += drawNs; traceBytes += u.length
        tracePixels += dirty.width().toLong() * dirty.height()
        if (now - traceStartNs < 10_000_000_000L) return
        val tiles = synchronized(tileBitmaps) { Triple(traceTiles, traceTileNs, traceTileMaxNs).also { traceTiles = 0; traceTileNs = 0; traceTileMaxNs = 0 } }
        Log.i(TAG, ("%d updates in %.1f s (%.1f/s), %d KB avg, %d%% of the screen avg, decode %.1f ms, draw %.1f ms; " +
            "%d tiles, %.1f ms avg, %.1f ms max")
            .format(traceFrames, (now - traceStartNs) / 1e9, traceFrames * 1e9 / (now - traceStartNs),
                traceBytes / traceFrames / 1024, tracePixels * 100 / traceFrames / (config.width.toLong() * config.height),
                traceDecodeNs / 1e6 / traceFrames, traceDrawNs / 1e6 / traceFrames,
                tiles.first, tiles.second / 1e6 / maxOf(1, tiles.first), tiles.third / 1e6))
        traceFrames = 0; traceDecodeNs = 0; traceDrawNs = 0; traceBytes = 0; tracePixels = 0
    }

    override fun release() {
        synchronized(lock) {
            if (released) return
            released = true
            queue.clear()
            lock.notifyAll()
        }
        // The Surface dies when surfaceDestroyed() returns: wait until the current update is drawn.
        if (Thread.currentThread() !== thread) runCatching { thread.join(1000) }
        workers.shutdown()
    }

    companion object {
        private const val TAG = "JpegDecoder"
        const val MAX_TILES = 64
    }
}
