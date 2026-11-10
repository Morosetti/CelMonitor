package com.celmonitor.display

import android.annotation.TargetApi
import android.content.Context
import android.graphics.Point
import android.hardware.display.DisplayManager
import android.media.MediaCodecList
import android.media.MediaCodecInfo
import android.os.Build
import android.util.Log
import android.view.Display
import android.view.Surface
import android.view.WindowManager
import com.celmonitor.protocol.Codec
import com.celmonitor.protocol.DisplayMode
import com.celmonitor.protocol.Proto
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/** Physical screen facts plus the stream modes this phone can actually decode. */
data class ScreenInfo(
    val widthPx: Int,          // natural orientation
    val heightPx: Int,
    val densityDpi: Int,
    val refreshRateMilliHz: Int,
    val rotation: Int,
    val modes: List<DisplayMode>,
    val codecs: Set<Codec>,
    val lowLatencyDecoder: Boolean,
)

object DeviceDisplay {
    private const val TAG = "DeviceDisplay"
    /** JPEG pictures are decoded on the CPU: more would only waste battery on the devices that need this path. */
    const val JPEG_MAX_FPS = 30

    fun query(context: Context): ScreenInfo {
        @Suppress("DEPRECATION")
        val display = if (Build.VERSION.SDK_INT >= 17)
            (context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager).getDisplay(Display.DEFAULT_DISPLAY)
        else (context.getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay
        val rotation = display.rotation
        val sideways = rotation == Surface.ROTATION_90 || rotation == Surface.ROTATION_270
        val dpi = context.resources.displayMetrics.densityDpi

        // Hardware decoding uses the asynchronous MediaCodec API with a callback handler (Android 6+); older
        // devices get JPEG pictures.
        val decoders = if (Build.VERSION.SDK_INT >= 23) Codec.entries.filter { it != Codec.JPEG }
            .mapNotNull { c -> findDecoder(c)?.let { c to it } }.toMap() else emptyMap()

        if (decoders.isEmpty() || Build.VERSION.SDK_INT < 23) {
            val raw = rawSize(display)
            val usable = Point().also { display.getSize(it) }
            // Android 4.0-4.3 tablets can never hide their system bar: the stream must fit the space left above it,
            // in either orientation (the bar stays at the bottom edge). From 4.4 on the app hides the bars.
            val bar = if (Build.VERSION.SDK_INT >= 19) 0 else maxOf(raw.y - usable.y, raw.x - usable.x, 0)
            val long = max(raw.x, raw.y)
            val short = min(raw.x, raw.y)
            val w = if (sideways) raw.y else raw.x
            val h = if (sideways) raw.x else raw.y
            val modes = buildJpegModes(long, short, bar)
            val refresh = (display.refreshRate * 1000).roundToInt()
            Log.i(TAG, "screen ${w}x$h (bar $bar px) dpi=$dpi refresh=$refresh: JPEG modes=$modes")
            return ScreenInfo(w, h, dpi, refresh, rotation, modes, setOf(Codec.JPEG), false)
        }

        // Mode sizes are reported in the natural orientation already.
        val mode = display.mode
        var w = mode.physicalWidth
        var h = mode.physicalHeight
        val refreshHz = mode.refreshRate
        if (w <= 0 || h <= 0) {
            val m = context.resources.displayMetrics
            w = m.widthPixels; h = m.heightPixels
        }
        val refresh = (refreshHz * 1000).roundToInt()

        val lowLatency = Build.VERSION.SDK_INT >= 30 && decoders[Codec.H264]?.let { info ->
            info.getCapabilitiesForType(Codec.H264.mime)
                .isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)
        } == true

        val maxFps = max(30, min(Proto.MAX_FPS, refreshHz.roundToInt()))
        val modes = buildModes(w, h, maxFps, decoders)
        Log.i(TAG, "screen ${w}x$h dpi=$dpi refresh=$refresh codecs=${decoders.keys} modes=$modes")
        return ScreenInfo(w, h, dpi, refresh, rotation, modes, decoders.keys, lowLatency)
    }

    /** Whole panel in the current orientation (getRealSize is Android 4.2+; before that a hidden method had it). */
    private fun rawSize(display: Display): Point {
        val p = Point()
        if (Build.VERSION.SDK_INT >= 17) { display.getRealSize(p); return p }
        try {
            p.x = Display::class.java.getMethod("getRawWidth").invoke(display) as Int
            p.y = Display::class.java.getMethod("getRawHeight").invoke(display) as Int
        } catch (e: Exception) {
            display.getSize(p)
        }
        return p
    }

    /**
     * JPEG modes: the usable area in landscape and portrait (exact fit, no scaling on the device), plus a 3/4 scale
     * version of each, which decodes almost twice as fast on a slow CPU (smoother, less battery, a bit softer).
     */
    fun buildJpegModes(long: Int, short: Int, bar: Int): List<DisplayMode> {
        val result = ArrayList<DisplayMode>()
        for ((cw, ch) in listOf(long to short - bar, short to long - bar)) {
            for (scale in doubleArrayOf(1.0, 0.75)) {
                val mw = even((cw * scale).roundToInt())
                val mh = even((ch * scale).roundToInt())
                if (Proto.validDimension(mw) && Proto.validDimension(mh)) result += DisplayMode(mw, mh, JPEG_MAX_FPS, Codec.JPEG.maskBit)
            }
        }
        return result
    }

    /** Prefers hardware decoders; software decoders are too slow for a display stream. */
    @TargetApi(23)
    fun findDecoder(codec: Codec): MediaCodecInfo? {
        val all = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
            .filter { !it.isEncoder && it.supportedTypes.any { t -> t.equals(codec.mime, ignoreCase = true) } }
        val hw = all.filter { isHardware(it) }
        return hw.firstOrNull() ?: all.firstOrNull()
    }

    @TargetApi(23)
    private fun isHardware(info: MediaCodecInfo): Boolean {
        if (Build.VERSION.SDK_INT >= 29) return info.isHardwareAccelerated
        val n = info.name.lowercase()
        return !(n.startsWith("omx.google.") || n.startsWith("c2.android.") || n.contains("sw"))
    }

    /**
     * Candidate modes, landscape and portrait, all without distortion:
     *  - the native panel resolution and scaled-down versions with the same aspect ratio;
     *  - standard 16:9 modes (shown letterboxed on phones that are not 16:9).
     * Each one is kept only if some decoder can handle it.
     */
    @TargetApi(23)
    fun buildModes(w: Int, h: Int, maxFps: Int, decoders: Map<Codec, MediaCodecInfo>): List<DisplayMode> {
        val long = max(w, h)
        val short = min(w, h)
        val aspect = long.toDouble() / short

        val landscape = linkedSetOf<Pair<Int, Int>>()
        landscape += even(long) to even(short)
        for (s in intArrayOf(1080, 900, 720)) if (s < short) landscape += align8((s * aspect).roundToInt()) to s
        for ((lw, lh) in listOf(1920 to 1080, 1600 to 900, 1280 to 720)) if (lw <= long) landscape += lw to lh

        val candidates = landscape.flatMap { (a, b) -> listOf(a to b, b to a) }
        val result = ArrayList<DisplayMode>()
        for ((cw, ch) in candidates) {
            if (!Proto.validDimension(cw) || !Proto.validDimension(ch)) continue
            var mask = 0
            var fps = 0
            for ((codec, info) in decoders) {
                val caps = info.getCapabilitiesForType(codec.mime).videoCapabilities ?: continue
                val f = when {
                    caps.areSizeAndRateSupported(cw, ch, maxFps.toDouble()) -> maxFps
                    caps.areSizeAndRateSupported(cw, ch, 30.0) -> 30
                    else -> 0
                }
                if (f > 0) { mask = mask or codec.maskBit; fps = max(fps, f) }
            }
            if (mask != 0) result += DisplayMode(cw, ch, fps, mask)
            if (result.size == Proto.MAX_MODES) break
        }
        return result
    }

    private fun even(v: Int) = v and 1.inv()
    private fun align8(v: Int) = (v + 4) / 8 * 8
}
