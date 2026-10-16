package com.celmonitor.input

import android.view.MotionEvent
import com.celmonitor.protocol.TouchAction
import com.celmonitor.protocol.TouchContact
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class TouchMapperTest {

    private fun map(action: Int, index: Int, vararg p: Triple<Int, Float, Float>) = TouchMapper.map(
        action, index, p.map { it.first }.toIntArray(), p.map { it.second }.toFloatArray(),
        p.map { it.third }.toFloatArray(), 1001, 501,
    )

    @Test
    fun corners_and_centre_are_normalised() {
        assertEquals(listOf(TouchContact(0, TouchAction.DOWN, 0, 0)), map(MotionEvent.ACTION_DOWN, 0, Triple(0, 0f, 0f)))
        assertEquals(listOf(TouchContact(0, TouchAction.MOVE, 65535, 65535)),
            map(MotionEvent.ACTION_MOVE, 0, Triple(0, 1000f, 500f)))
        assertEquals(listOf(TouchContact(0, TouchAction.UP, 32767, 32767)),
            map(MotionEvent.ACTION_UP, 0, Triple(0, 500f, 250f)))
    }

    @Test
    fun outside_the_view_is_clamped() {
        val c = map(MotionEvent.ACTION_MOVE, 0, Triple(0, -20f, 9000f)).single()
        assertEquals(0, c.x)
        assertEquals(65535, c.y)
    }

    @Test
    fun second_finger_down_and_up() {
        val down = map(MotionEvent.ACTION_POINTER_DOWN, 1, Triple(0, 10f, 10f), Triple(1, 20f, 20f))
        assertEquals(listOf(TouchAction.MOVE, TouchAction.DOWN), down.map { it.action })
        val up = map(MotionEvent.ACTION_POINTER_UP, 0, Triple(0, 10f, 10f), Triple(1, 20f, 20f))
        assertEquals(listOf(TouchAction.UP, TouchAction.MOVE), up.map { it.action })
    }

    @Test
    fun cancel_and_ids_out_of_range() {
        assertEquals(listOf(TouchAction.CANCEL), map(MotionEvent.ACTION_CANCEL, 0, Triple(3, 1f, 1f)).map { it.action })
        assertEquals(listOf(9), map(MotionEvent.ACTION_MOVE, 0, Triple(9, 1f, 1f), Triple(10, 1f, 1f)).map { it.id })
        assertTrue(map(MotionEvent.ACTION_HOVER_MOVE, 0, Triple(0, 1f, 1f)).isEmpty())
    }
}
