package com.celmonitor.input

import android.view.MotionEvent
import com.celmonitor.protocol.Proto
import com.celmonitor.protocol.TouchAction
import com.celmonitor.protocol.TouchContact

/**
 * Turns the fingers on the video into INPUT_TOUCH contacts: every finger currently on the screen with its action,
 * coordinates normalised to 0..65535 over the video area (independent of the phone and stream resolutions).
 * The PC injects them as Windows touch input on the virtual monitor.
 */
object TouchMapper {

    fun map(e: MotionEvent, width: Int, height: Int): List<TouchContact> {
        val n = e.pointerCount
        val ids = IntArray(n) { e.getPointerId(it) }
        val xs = FloatArray(n) { e.getX(it) }
        val ys = FloatArray(n) { e.getY(it) }
        return map(e.actionMasked, e.actionIndex, ids, xs, ys, width, height)
    }

    /** Pure version of [map] (unit-testable): pointer ids, view-relative coordinates in pixels. */
    fun map(
        actionMasked: Int, actionIndex: Int, ids: IntArray, xs: FloatArray, ys: FloatArray, width: Int, height: Int,
    ): List<TouchContact> {
        if (width <= 0 || height <= 0) return emptyList()
        val out = ArrayList<TouchContact>(ids.size)
        for (i in ids.indices) {
            val id = ids[i]
            if (id < 0 || id >= Proto.MAX_TOUCH_CONTACTS) continue  // 11th finger and beyond: ignored
            val action = when (actionMasked) {
                MotionEvent.ACTION_DOWN -> TouchAction.DOWN
                MotionEvent.ACTION_POINTER_DOWN -> if (i == actionIndex) TouchAction.DOWN else TouchAction.MOVE
                MotionEvent.ACTION_UP -> TouchAction.UP
                MotionEvent.ACTION_POINTER_UP -> if (i == actionIndex) TouchAction.UP else TouchAction.MOVE
                MotionEvent.ACTION_MOVE -> TouchAction.MOVE
                MotionEvent.ACTION_CANCEL -> TouchAction.CANCEL
                else -> return emptyList()
            }
            out += TouchContact(id, action, normalize(xs[i], width), normalize(ys[i], height))
        }
        return out
    }

    private fun normalize(v: Float, size: Int): Int =
        if (size <= 1) 0 else (v / (size - 1) * 65535f).toInt().coerceIn(0, 65535)
}
