package com.celmonitor.display

import com.celmonitor.protocol.Codec
import com.celmonitor.protocol.DisplayMode
import org.junit.Assert.assertEquals
import org.junit.Test

class DeviceDisplayTest {
    @Test fun jpegModesLeaveRoomForTheTabletSystemBar() {
        // Galaxy Tab 2 10.1 (Android 4.0): 1280x800 panel, 48 px system bar that can never be hidden.
        val bit = Codec.JPEG.maskBit
        assertEquals(
            listOf(
                DisplayMode(1280, 752, DeviceDisplay.JPEG_MAX_FPS, bit), DisplayMode(960, 564, DeviceDisplay.JPEG_MAX_FPS, bit),
                DisplayMode(800, 1232, DeviceDisplay.JPEG_MAX_FPS, bit), DisplayMode(600, 924, DeviceDisplay.JPEG_MAX_FPS, bit),
            ),
            DeviceDisplay.buildJpegModes(1280, 800, 48),
        )
    }

    @Test fun jpegModesWithoutBar() {
        val modes = DeviceDisplay.buildJpegModes(1920, 1080, 0)
        assertEquals(DisplayMode(1920, 1080, DeviceDisplay.JPEG_MAX_FPS, Codec.JPEG.maskBit), modes[0])
        assertEquals(4, modes.size)
    }
}
