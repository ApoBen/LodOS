package org.lodos.app.lite

import android.app.ActivityManager
import android.content.Context
import android.os.Build
import android.system.Os
import android.system.OsConstants
import java.io.File

/** Android glue: gather DeviceSpec. No decision logic here. */
object AndroidDeviceSpec {
    fun read(context: Context, gpuRenderer: String = ""): DeviceSpec {
        val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val mi = ActivityManager.MemoryInfo().also { am.getMemoryInfo(it) }
        val soc = if (Build.VERSION.SDK_INT >= 31) "${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}" else Build.HARDWARE
        return DeviceSpec(
            ramMb = (mi.totalMem / (1024 * 1024)).toInt(),
            cpuCores = Runtime.getRuntime().availableProcessors(),
            maxFreqMhz = maxFreqMhz(),
            socModel = soc,
            gpuRenderer = gpuRenderer,
            pageSize = Os.sysconf(OsConstants._SC_PAGESIZE).toInt(),
        )
    }

    private fun maxFreqMhz(): Int = (File("/sys/devices/system/cpu").listFiles() ?: emptyArray())
        .filter { it.name.matches(Regex("cpu\\d+")) }
        .mapNotNull { runCatching { File(it, "cpufreq/cpuinfo_max_freq").readText().trim().toInt() / 1000 }.getOrNull() }
        .maxOrNull() ?: 0
}
