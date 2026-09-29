package com.celmonitor.display

import android.content.Context
import android.hardware.display.DisplayManager
import android.media.MediaCodecList
import android.media.MediaCodecInfo
import android.os.Build
import android.util.Log
import android.view.Display
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

    fun query(context: Context): ScreenInfo {
        val dm = context.getSystemService(DisplayManager::class.java)
        val display = dm.getDisplay(Display.DEFAULT_DISPLAY)
        val mode = display.mode
        val rotation = display.rotation
        // Mode sizes are reported in the natural orientation already.
        var w = mode.physicalWidth
        var h = mode.physicalHeight
        if (w <= 0 || h <= 0) {
            val m = context.resources.displayMetrics
            w = m.widthPixels; h = m.heightPixels
        }
        val dpi = context.resources.displayMetrics.densityDpi
        val refresh = (mode.refreshRate * 1000).roundToInt()

        val decoders = Codec.entries.mapNotNull { c -> findDecoder(c)?.let { c to it } }.toMap()
        val lowLatency = Build.VERSION.SDK_INT >= 30 && decoders[Codec.H264]?.let { info ->
            info.getCapabilitiesForType(Codec.H264.mime)
                .isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)
        } == true

        val maxFps = max(30, min(Proto.MAX_FPS, mode.refreshRate.roundToInt()))
        val modes = buildModes(w, h, maxFps, decoders)
        Log.i(TAG, "screen ${w}x$h dpi=$dpi refresh=$refresh codecs=${decoders.keys} modes=$modes")
        return ScreenInfo(w, h, dpi, refresh, rotation, modes, decoders.keys, lowLatency)
    }

    /** Prefers hardware decoders; software decoders are too slow for a display stream. */
    fun findDecoder(codec: Codec): MediaCodecInfo? {
        val all = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
            .filter { !it.isEncoder && it.supportedTypes.any { t -> t.equals(codec.mime, ignoreCase = true) } }
        val hw = all.filter { isHardware(it) }
        return hw.firstOrNull() ?: all.firstOrNull()
    }

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
