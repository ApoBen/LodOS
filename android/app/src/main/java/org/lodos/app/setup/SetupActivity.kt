package org.lodos.app.setup

import android.app.Activity
import android.content.Intent
import android.content.ComponentName
import android.content.ServiceConnection
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import org.lodos.app.DisplayActivity
import org.lodos.app.MainActivity
import org.lodos.app.R
import org.lodos.app.bugreport.BugReportCollector
import org.lodos.app.session.GuestSessionService
import org.lodos.app.session.SessionController
import org.lodos.app.session.SessionState
import org.lodos.app.session.SessionUiModel
import java.io.File
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * P1 setup wizard launcher activity: Welcome -> Rootfs status -> Start.
 */
class SetupActivity : Activity() {

    private lateinit var stepIndicator: TextView
    private lateinit var titleView: TextView
    private lateinit var bodyView: TextView
    private lateinit var rootfsContainer: LinearLayout
    private lateinit var rootfsStatusView: TextView
    private lateinit var rootfsProgress: ProgressBar
    private lateinit var rootfsErrorView: TextView
    private lateinit var startContainer: LinearLayout
    private lateinit var nativeInfoButton: Button
    private lateinit var backButton: Button
    private lateinit var primaryButton: Button
    private lateinit var sessionStatusView: TextView
    private lateinit var sessionStartButton: Button
    private lateinit var sessionStopButton: Button

    private var bound = false
    private var pendingSessionStart = false
    private var sessionController: SessionController? = null
    private val sessionListener: (SessionState) -> Unit = { state ->
        runOnUiThread { if (!isFinishing && !isDestroyed) renderSession(state) }
    }
    private val sessionConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, service: IBinder) {
            val controller = (service as GuestSessionService.LocalBinder).controller
            sessionController = controller
            controller.addListener(sessionListener)
            renderSession(controller.state)
        }

        override fun onServiceDisconnected(name: ComponentName) {
            sessionController?.removeListener(sessionListener)
            sessionController = null
            renderSession(SessionState.Stopped)
        }
    }

    private lateinit var repo: RootfsRepo
    private lateinit var machine: SetupStateMachine
    private lateinit var installer: RootfsInstaller
    private val worker: ExecutorService = Executors.newSingleThreadExecutor()

    private val stateListener: (SetupState) -> Unit = { state ->
        runOnUiThread {
            if (!isFinishing && !isDestroyed) {
                render(state)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_setup)

        stepIndicator = findViewById(R.id.setup_step_indicator)
        titleView = findViewById(R.id.setup_title)
        bodyView = findViewById(R.id.setup_body)
        rootfsContainer = findViewById(R.id.setup_rootfs_container)
        rootfsStatusView = findViewById(R.id.setup_rootfs_status)
        rootfsProgress = findViewById(R.id.setup_rootfs_progress)
        rootfsErrorView = findViewById(R.id.setup_rootfs_error)
        startContainer = findViewById(R.id.setup_start_container)
        nativeInfoButton = findViewById(R.id.setup_btn_native_info)
        backButton = findViewById(R.id.setup_btn_back)
        primaryButton = findViewById(R.id.setup_btn_primary)
        sessionStatusView = findViewById(R.id.setup_session_status)
        sessionStartButton = findViewById(R.id.setup_btn_session_start)
        sessionStopButton = findViewById(R.id.setup_btn_session_stop)

        val isDebuggable = (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0
        repo = if (isDebuggable && intent.getStringExtra("fake_repo") == "1") {
            FakeRootfsRepo(initiallyInstalled = isRootfsPresentOnDisk(), stepDelayMs = 150L)
        } else HttpRootfsRepo(this)
        val store = SharedPreferencesSetupStore(
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        )
        machine = SetupStateMachine(
            store = store,
            rootfsInstalled = { repo.isInstalled() || isRootfsPresentOnDisk() }
        )
        installer = RootfsInstaller(
            machine = machine,
            repo = repo,
            spec = if (repo is HttpRootfsRepo) HttpRootfsRepo.rootfsSpec(this) else RootfsSpec("", "", "test")
        )

        backButton.setOnClickListener {
            machine.dispatch(SetupEvent.Back)
        }
        primaryButton.setOnClickListener {
            onPrimaryAction(machine.state)
        }
        findViewById<Button>(R.id.setup_btn_report).setOnClickListener {
            BugReportCollector.reportProblem(this, machine.state.toString())
        }
        nativeInfoButton.setOnClickListener {
            startActivity(Intent(this, MainActivity::class.java))
        }
        sessionStartButton.setOnClickListener { startGuestSession() }
        sessionStopButton.setOnClickListener { GuestSessionService.stop(this) }

        machine.addListener(stateListener)
    }

    override fun onDestroy() {
        super.onDestroy()
        machine.removeListener(stateListener)
        installer.cancel()
        worker.shutdownNow()
    }

    override fun onStart() {
        super.onStart()
        bound = bindService(Intent(this, GuestSessionService::class.java), sessionConnection, BIND_AUTO_CREATE)
    }

    override fun onStop() {
        sessionController?.removeListener(sessionListener)
        sessionController = null
        if (bound) {
            unbindService(sessionConnection)
            bound = false
        }
        super.onStop()
    }

    private fun startGuestSession() {
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            pendingSessionStart = true
            requestPermissions(arrayOf(android.Manifest.permission.POST_NOTIFICATIONS), REQUEST_NOTIFICATIONS)
            return
        }
        GuestSessionService.start(this)
    }

    @Deprecated("Deprecated by Android; permission result is still delivered for this Activity API")
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_NOTIFICATIONS && pendingSessionStart) {
            pendingSessionStart = false
            // Start either way: Android permits foreground services when this permission is denied.
            GuestSessionService.start(this)
        }
    }

    private fun renderSession(state: SessionState) {
        val ui = SessionUiModel.from(state)
        sessionStatusView.text = "Session: ${ui.status}"
        sessionStartButton.text = ui.startLabel
        sessionStartButton.isEnabled = ui.startEnabled
        sessionStopButton.text = ui.stopLabel
        sessionStopButton.isEnabled = ui.stopEnabled
    }

    private fun onPrimaryAction(state: SetupState) {
        when (state.step) {
            SetupStep.WELCOME -> machine.dispatch(SetupEvent.ContinueFromWelcome)
            SetupStep.ROOTFS -> when (state.rootfsStatus) {
                RootfsStatus.MISSING, RootfsStatus.ERROR -> {
                    worker.execute { installer.install() }
                }
                RootfsStatus.READY -> machine.dispatch(SetupEvent.ProceedToStart)
                RootfsStatus.DOWNLOADING, RootfsStatus.EXTRACTING -> Unit
            }
            SetupStep.START -> {
                startActivity(Intent(this, DisplayActivity::class.java))
            }
        }
    }

    private fun render(state: SetupState) {
        when (state.step) {
            SetupStep.WELCOME -> {
                stepIndicator.setText(R.string.setup_step_welcome)
                titleView.setText(R.string.setup_welcome_title)
                bodyView.setText(R.string.setup_welcome_body)
                rootfsContainer.visibility = View.GONE
                startContainer.visibility = View.GONE
                backButton.visibility = View.GONE
                primaryButton.isEnabled = true
                primaryButton.setText(R.string.setup_btn_continue)
            }

            SetupStep.ROOTFS -> {
                stepIndicator.setText(R.string.setup_step_rootfs)
                titleView.setText(R.string.setup_rootfs_title)
                bodyView.setText(R.string.setup_rootfs_body)
                rootfsContainer.visibility = View.VISIBLE
                startContainer.visibility = View.GONE
                backButton.visibility = View.VISIBLE
                backButton.isEnabled = !state.isBusy
                rootfsProgress.progress = state.progress

                when (state.rootfsStatus) {
                    RootfsStatus.MISSING -> {
                        rootfsStatusView.setText(R.string.setup_status_missing)
                        primaryButton.isEnabled = true
                        primaryButton.setText(R.string.setup_btn_install)
                    }
                    RootfsStatus.DOWNLOADING -> {
                        rootfsStatusView.text = getString(R.string.setup_status_downloading, state.progress)
                        primaryButton.isEnabled = false
                        primaryButton.setText(R.string.setup_btn_installing)
                    }
                    RootfsStatus.EXTRACTING -> {
                        rootfsStatusView.text = getString(R.string.setup_status_extracting, state.progress)
                        primaryButton.isEnabled = false
                        primaryButton.setText(R.string.setup_btn_installing)
                    }
                    RootfsStatus.READY -> {
                        rootfsStatusView.setText(R.string.setup_status_ready)
                        primaryButton.isEnabled = true
                        primaryButton.setText(R.string.setup_btn_continue)
                    }
                    RootfsStatus.ERROR -> {
                        rootfsStatusView.setText(R.string.setup_status_error)
                        primaryButton.isEnabled = true
                        primaryButton.setText(R.string.setup_btn_retry)
                    }
                }

                if (state.errorMessage.isNullOrBlank()) {
                    rootfsErrorView.visibility = View.GONE
                    rootfsErrorView.text = ""
                } else {
                    rootfsErrorView.visibility = View.VISIBLE
                    rootfsErrorView.text = state.errorMessage
                }
            }

            SetupStep.START -> {
                stepIndicator.setText(R.string.setup_step_start)
                titleView.setText(R.string.setup_start_title)
                bodyView.setText(R.string.setup_start_body)
                rootfsContainer.visibility = View.GONE
                startContainer.visibility = View.VISIBLE
                backButton.visibility = View.VISIBLE
                backButton.isEnabled = true
                primaryButton.isEnabled = true
                primaryButton.setText(R.string.setup_btn_start)
            }
        }
    }

    private fun isRootfsPresentOnDisk(): Boolean {
        val rootfsDir = File(filesDir, "rootfs")
        return File(rootfsDir, "etc/debian_version").exists() && File(rootfsDir, "bin/sh").exists()
    }

    companion object {
        private const val REQUEST_NOTIFICATIONS = 5201
        private const val PREFS_NAME = "lodos_setup"
    }
}
