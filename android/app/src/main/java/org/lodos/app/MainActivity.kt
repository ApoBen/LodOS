package org.lodos.app

import android.app.Activity
import android.os.Bundle
import android.view.Menu
import android.view.MenuItem
import android.widget.TextView
import org.lodos.app.bugreport.BugReportCollector

class MainActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        val statusText = findViewById<TextView>(R.id.status_text)
        statusText.text = stringFromJNI()
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        BugReportCollector.addMenuItem(menu)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean =
        BugReportCollector.handleMenuItem(this, item, "main activity") || super.onOptionsItemSelected(item)

    private external fun stringFromJNI(): String

    companion object {
        init {
            System.loadLibrary("lodos_native")
        }
    }
}
