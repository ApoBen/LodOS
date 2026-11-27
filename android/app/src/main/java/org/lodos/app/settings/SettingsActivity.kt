package org.lodos.app.settings

import android.app.Activity
import android.os.Bundle
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView

class SettingsActivity : Activity() {
    private val store by lazy { LodosSettingsStore(this) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        title = "Settings"
        val padding = (20 * resources.displayMetrics.density).toInt()
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padding, padding, padding, padding)
        }
        val current = store.read()
        val liteChoices = listOf("auto", "on", "off")
        val presetChoices = listOf("off", "quality", "balanced", "performance")
        val lite = addSpinner(layout, "Lite mode", liteChoices, current.liteMode)
        val preset = addSpinner(layout, "Graphics upscale preset", presetChoices, current.fgPreset)
        val sharpnessLabel = TextView(this).apply { text = "Upscale sharpness: ${current.fgSharpness}" }
        layout.addView(sharpnessLabel)
        val sharpness = SeekBar(this).apply {
            max = 100
            progress = (current.fgSharpness.coerceIn(0f, 1f) * 100).toInt()
        }
        layout.addView(sharpness, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        fun save() {
            store.write(LodosSettings(lite.selectedItem.toString(), preset.selectedItem.toString(), sharpness.progress / 100f))
        }
        lite.onItemSelected = { save() }
        preset.onItemSelected = { save() }
        sharpness.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(bar: SeekBar?, value: Int, fromUser: Boolean) {
                sharpnessLabel.text = "Upscale sharpness: ${value / 100f}"
                if (fromUser) save()
            }
            override fun onStartTrackingTouch(bar: SeekBar?) = Unit
            override fun onStopTrackingTouch(bar: SeekBar?) = Unit
        })
        setContentView(layout)
    }

    private fun addSpinner(parent: LinearLayout, label: String, choices: List<String>, selected: String): Spinner {
        parent.addView(TextView(this).apply { text = label })
        val spinner = Spinner(this)
        spinner.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, choices)
        spinner.setSelection(choices.indexOf(selected).takeIf { it >= 0 } ?: 0)
        parent.addView(spinner)
        return spinner
    }

    private var Spinner.onItemSelected: (() -> Unit)?
        get() = tag as? (() -> Unit)
        set(value) {
            tag = value
            onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
                override fun onNothingSelected(parent: android.widget.AdapterView<*>?) = Unit
                override fun onItemSelected(parent: android.widget.AdapterView<*>?, view: android.view.View?, position: Int, id: Long) {
                    (tag as? (() -> Unit))?.invoke()
                }
            }
        }
}
