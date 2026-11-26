package org.lodos.app

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.view.MotionEvent
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager

/** Pure-black fullscreen overlay (AMOLED pixels off) used by the dev keeper during long adb work. Any touch closes it. */
class BlackActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.attributes = window.attributes.apply { screenBrightness = 0f }
        setContentView(View(this).apply { setBackgroundColor(Color.BLACK) })
        window.decorView.setBackgroundColor(Color.BLACK)
        window.insetsController?.let {
            it.hide(WindowInsets.Type.systemBars())
            it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.action == MotionEvent.ACTION_DOWN) finish()
        return true
    }

    override fun onStop() {
        super.onStop()
        finish() // screen off (power key) or another app on top -> go away
    }
}
