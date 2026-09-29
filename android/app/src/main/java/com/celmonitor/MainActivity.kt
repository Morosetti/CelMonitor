package com.celmonitor

import android.app.Activity
import android.app.AlertDialog
import android.content.pm.ActivityInfo
import android.os.Build
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.Button
import android.widget.ImageButton
import android.widget.PopupMenu
import android.widget.TextView
import android.widget.Toast
import com.celmonitor.protocol.ClientSettings
import com.celmonitor.protocol.Notice
import com.celmonitor.session.ConnectionManager
import com.celmonitor.session.ConnectionManager.State
import com.celmonitor.ui.AspectFrameLayout

class MainActivity : Activity(), ConnectionManager.Listener, SurfaceHolder.Callback {

    private val connections get() = (application as App).connections

    private lateinit var videoFrame: AspectFrameLayout
    private lateinit var surfaceView: SurfaceView
    private lateinit var statusPanel: View
    private lateinit var statusText: TextView
    private lateinit var detailText: TextView
    private lateinit var errorText: TextView
    private lateinit var resumeButton: Button
    private lateinit var menuButton: ImageButton

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        videoFrame = findViewById(R.id.videoFrame)
        surfaceView = findViewById(R.id.surface)
        statusPanel = findViewById(R.id.statusPanel)
        statusText = findViewById(R.id.statusText)
        detailText = findViewById(R.id.detailText)
        errorText = findViewById(R.id.errorText)
        resumeButton = findViewById(R.id.resumeButton)
        menuButton = findViewById(R.id.menuButton)

        surfaceView.holder.addCallback(this)
        menuButton.setOnClickListener { showMenu() }
        resumeButton.setOnClickListener { connections.start() }
    }

    override fun onStart() {
        super.onStart()
        connections.listener = this
        if (connections.state !is State.Paused) connections.start()
    }

    override fun onStop() {
        connections.listener = null
        super.onStop()
    }

    // ------------------------------------------------------------------ state → UI

    override fun onState(state: State) {
        when (state) {
            is State.Waiting -> {
                showStatus(getString(R.string.status_waiting), getString(R.string.hint_waiting), state.lastError)
            }
            is State.Paused -> {
                showStatus(getString(R.string.status_paused), "", null)
                resumeButton.visibility = View.VISIBLE
            }
            is State.Connected -> {
                showStatus(getString(R.string.status_connected), "${state.hostName} · ${state.transport}", null)
            }
            is State.Streaming -> enterMonitorMode(state)
        }
    }

    private fun showStatus(title: String, detail: String, error: String?) {
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        videoFrame.visibility = View.GONE
        menuButton.visibility = View.GONE
        statusPanel.visibility = View.VISIBLE
        resumeButton.visibility = View.GONE
        statusText.text = title
        detailText.text = detail
        errorText.text = error ?: ""
        errorText.visibility = if (error.isNullOrBlank()) View.GONE else View.VISIBLE
    }

    private fun enterMonitorMode(s: State.Streaming) {
        val c = s.config
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        requestedOrientation = if (c.width >= c.height) ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        else ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
        hideSystemBars()
        statusPanel.visibility = View.GONE
        videoFrame.setAspect(c.width, c.height)
        videoFrame.visibility = View.VISIBLE
        menuButton.visibility = View.VISIBLE
    }

    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_STABLE)
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus && connections.state is State.Streaming) hideSystemBars()
    }

    override fun onNotice(notice: Notice) {
        Toast.makeText(this, notice.message, if (notice.severity >= 1) Toast.LENGTH_LONG else Toast.LENGTH_SHORT).show()
    }

    // ------------------------------------------------------------------ surface

    override fun surfaceCreated(holder: SurfaceHolder) = connections.attachSurface(holder.surface)
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
    override fun surfaceDestroyed(holder: SurfaceHolder) = connections.detachSurface()

    // ------------------------------------------------------------------ menu

    private fun showMenu() {
        val popup = PopupMenu(this, menuButton)
        val m = popup.menu
        m.add(0, 1, 0, R.string.menu_exit)
        m.addSubMenu(0, 2, 1, R.string.menu_orientation).apply {
            add(1, 21, 0, R.string.menu_landscape)
            add(1, 22, 1, R.string.menu_portrait)
        }
        m.addSubMenu(0, 3, 2, R.string.menu_quality).apply {
            add(2, 31, 0, R.string.menu_quality_low)
            add(2, 32, 1, R.string.menu_quality_medium)
            add(2, 33, 2, R.string.menu_quality_high)
        }
        m.addSubMenu(0, 4, 3, R.string.menu_fps).apply {
            add(3, 41, 0, "30")
            add(3, 42, 1, "60")
        }
        m.add(0, 5, 4, R.string.menu_info)
        popup.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                1 -> connections.pause()
                21 -> connections.sendSettings(ClientSettings(ClientSettings.MASK_ORIENTATION, 0, 0, 0))
                22 -> connections.sendSettings(ClientSettings(ClientSettings.MASK_ORIENTATION, 0, 0, 1))
                31 -> connections.sendSettings(ClientSettings(ClientSettings.MASK_QUALITY, 0, 30, 0))
                32 -> connections.sendSettings(ClientSettings(ClientSettings.MASK_QUALITY, 0, 60, 0))
                33 -> connections.sendSettings(ClientSettings(ClientSettings.MASK_QUALITY, 0, 90, 0))
                41 -> connections.sendSettings(ClientSettings(ClientSettings.MASK_FPS, 30, 0, 0))
                42 -> connections.sendSettings(ClientSettings(ClientSettings.MASK_FPS, 60, 0, 0))
                5 -> showInfo()
                else -> return@setOnMenuItemClickListener false
            }
            true
        }
        popup.show()
    }

    private fun showInfo() {
        val s = connections.state as? State.Streaming ?: return
        val c = s.config
        val st = connections.stats
        val hs = connections.hostStatus
        val text = buildString {
            appendLine("PC: ${s.hostName}")
            appendLine("Conexão: ${s.transport}")
            appendLine("Resolução: ${c.width}×${c.height} @ ${c.fps} fps")
            appendLine("Codec: ${c.codec} · ${c.bitrateKbps} kbit/s alvo")
            appendLine("Decoder: ${connections.decoderName}${if (connections.decoderLowLatency) " (baixa latência)" else ""}")
            if (st != null) {
                appendLine("Recebendo: %.1f fps · %d kbit/s".format(st.receivedFps, st.receivedKbps))
                appendLine("Decodificação: %.1f ms · RTT USB: %.1f ms".format(st.renderUs / 1000.0, st.rttUs / 1000.0))
            }
            if (hs != null) {
                appendLine("Encoder no PC: ${hs.encoderName}")
                appendLine("Latência total aprox.: %.1f ms".format(hs.latencyUs / 1000.0))
            }
        }
        AlertDialog.Builder(this).setTitle(R.string.menu_info).setMessage(text).setPositiveButton(R.string.close, null).show()
    }
}
