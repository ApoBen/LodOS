package org.lodos.app.input

import org.junit.Assert.assertEquals
import org.junit.Test

class TouchMapperTest {
    private val two = listOf(TouchPointer(7, 10f, 20f), TouchPointer(13, 30f, 40f))

    @Test fun downUsesActionPointerId() {
        assertEquals(listOf(TouchSample(7, TouchMapper.DOWN, 10f, 20f)), TouchMapper.map(TouchMapper.DOWN, 0, two, 1080, 2400))
    }
    @Test fun pointerDownKeepsDistinctId() {
        assertEquals(listOf(TouchSample(13, TouchMapper.DOWN, 30f, 40f)), TouchMapper.map(TouchMapper.POINTER_DOWN, 1, two, 1080, 2400))
    }
    @Test fun moveEmitsEveryPointer() {
        assertEquals(listOf(TouchSample(7, TouchMapper.MOVE, 10f, 20f), TouchSample(13, TouchMapper.MOVE, 30f, 40f)), TouchMapper.map(TouchMapper.MOVE, 0, two, 1080, 2400))
    }
    @Test fun pointerUpUsesActionPointerId() {
        assertEquals(listOf(TouchSample(13, TouchMapper.UP, 30f, 40f)), TouchMapper.map(TouchMapper.POINTER_UP, 1, two, 1080, 2400))
    }
    @Test fun cancelIsExplicit() {
        assertEquals(listOf(TouchSample(-1, TouchMapper.CANCEL, 0f, 0f)), TouchMapper.map(TouchMapper.CANCEL, 0, two, 1080, 2400))
    }
    @Test fun scalesSurfaceCoordinates() {
        assertEquals(TouchSample(7, TouchMapper.DOWN, 540f, 1200f), TouchMapper.map(TouchMapper.DOWN, 0, listOf(TouchPointer(7, 50f, 25f)), 100, 50).single())
    }
}
