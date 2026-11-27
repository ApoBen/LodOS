package org.lodos.app.input

/** Android MotionEvent values and pointers represented as plain data for JVM tests. */
data class TouchPointer(val id: Int, val x: Float, val y: Float)
data class TouchSample(val id: Int, val action: Int, val x: Float, val y: Float)

object TouchMapper {
    const val DOWN = 0
    const val UP = 1
    const val MOVE = 2
    const val CANCEL = 3
    const val POINTER_DOWN = 5
    const val POINTER_UP = 6

    fun map(action: Int, actionIndex: Int, pointers: List<TouchPointer>, width: Int, height: Int,
            outputWidth: Int = 1080, outputHeight: Int = 2400): List<TouchSample> {
        if (action == CANCEL) return listOf(TouchSample(-1, CANCEL, 0f, 0f))
        if (pointers.isEmpty() || width <= 0 || height <= 0) return emptyList()
        val selected = when (action) {
            DOWN, POINTER_DOWN, UP, POINTER_UP -> pointers.getOrNull(actionIndex)?.let(::listOf) ?: return emptyList()
            MOVE -> pointers
            else -> return emptyList()
        }
        val mappedAction = when (action) { DOWN, POINTER_DOWN -> DOWN; UP, POINTER_UP -> UP; else -> MOVE }
        return selected.map { p ->
            TouchSample(p.id, mappedAction, p.x * outputWidth / width, p.y * outputHeight / height)
        }
    }
}
