package org.lodos.app.session

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.Process
import android.util.Log
import java.io.File
import java.util.concurrent.Executors
import org.lodos.app.R
import org.lodos.app.guest.GuestLaunch
import org.lodos.app.guest.GuestPaths
import org.lodos.app.guest.GuestSpec
import org.lodos.app.lite.AndroidDeviceSpec
import org.lodos.app.lite.LiteMode
import org.lodos.app.lite.LiteModeStore

/**
 * Foreground service owning the guest (T048): proot -> /sbin/lodos-init (PID1) with lite env.
 * Notification has a Stop action. Logs: filesDir/logs/guest-<ts>.log (keep 5).
 */
class GuestSessionService : Service() {
    companion object {
        const val ACTION_START = "org.lodos.app.session.START"
        const val ACTION_STOP = "org.lodos.app.session.STOP"
        const val CHANNEL = "guest_session"
        const val NOTIF_ID = 0x10D0
        private const val TAG = "LodOS.Session"

        fun start(ctx: Context) {
            val i = Intent(ctx, GuestSessionService::class.java).setAction(ACTION_START)
            if (Build.VERSION.SDK_INT >= 26) ctx.startForegroundService(i) else ctx.startService(i)
        }

        fun stop(ctx: Context) {
            ctx.startService(Intent(ctx, GuestSessionService::class.java).setAction(ACTION_STOP))
        }
    }

    inner class LocalBinder : Binder() {
        val controller: SessionController get() = this@GuestSessionService.controller
    }

    private val worker = Executors.newSingleThreadExecutor()
    private val audioBridge by lazy {
        AudioBridge(JvmProcessRunner(), File(filesDir, "logs"), File(filesDir, "run"), applicationInfo.nativeLibraryDir)
    }
    lateinit var controller: SessionController
        private set
    private val listener: (SessionState) -> Unit = { s -> onState(s) }

    override fun onCreate() {
        super.onCreate()
        controller = SessionController(JvmProcessRunner(), SystemClock(), LogRotator(File(filesDir, "logs")))
        controller.addListener(listener)
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Guest session", NotificationManager.IMPORTANCE_LOW))
    }

    override fun onBind(intent: Intent?): IBinder = LocalBinder()

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> worker.execute {
                controller.stop()
                audioBridge.stop()
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
            }
            else -> {
                goForeground(controller.state)
                worker.execute {
                    runCatching {
                        audioBridge.start()
                        controller.start(buildSpec())
                    }
                        .onFailure { Log.e(TAG, "start failed", it) }
                }
            }
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        controller.removeListener(listener)
        worker.execute { controller.stop(); audioBridge.stop() }
        worker.shutdown()
        super.onDestroy()
    }

    fun buildSpec(): GuestSpec {
        val run = File(filesDir, "run").apply { mkdirs() }
        val tmp = File(cacheDir, "proot-tmp").apply { mkdirs() }
        val paths = GuestPaths(
            nativeLibDir = applicationInfo.nativeLibraryDir,
            filesDir = filesDir.absolutePath,
            rootfsDir = File(filesDir, "rootfs").absolutePath,
            runDir = run.absolutePath,
            tmpDir = tmp.absolutePath,
        )
        val lite = LiteMode.resolve(AndroidDeviceSpec.read(this), LiteModeStore(this).userForce)
        val audioSocket = File(run, AudioBridge.SOCKET_NAME)
        return GuestSpec(
            paths,
            lite = lite,
            lodosInit = true,
            extraBinds = listOf(audioSocket.absolutePath to GuestLaunch.GUEST_AUDIO_SOCKET),
        )
    }

    private fun onState(s: SessionState) {
        Log.i(TAG, "state=$s pid=${Process.myPid()}")
        if (s is SessionState.Stopped || s is SessionState.Failed) {
            audioBridge.stop()
            return
        }
        getSystemService(NotificationManager::class.java).notify(NOTIF_ID, notification(s))
    }

    private fun goForeground(s: SessionState) {
        val n = notification(s)
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        } else {
            startForeground(NOTIF_ID, n)
        }
    }

    private fun notification(s: SessionState): Notification {
        val stopPi = PendingIntent.getService(
            this, 1, Intent(this, GuestSessionService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_launcher)
            .setContentTitle("LodOS guest")
            .setContentText(s.toString())
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, "Stop", stopPi).build())
            .build()
    }
}
