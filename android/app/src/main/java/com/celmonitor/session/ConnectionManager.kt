package com.celmonitor.session

import android.annotation.SuppressLint
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.Log
import android.view.Surface
import com.celmonitor.BuildConfig
import com.celmonitor.display.DeviceDisplay
import com.celmonitor.protocol.ClientSettings
import com.celmonitor.protocol.DisconnectReason
import com.celmonitor.protocol.Hello
import com.celmonitor.protocol.HelloAck
import com.celmonitor.protocol.HostStatus
import com.celmonitor.protocol.InputMouse
import com.celmonitor.protocol.Notice
import com.celmonitor.protocol.Proto
import com.celmonitor.protocol.StreamConfig
import com.celmonitor.protocol.TouchContact
import com.celmonitor.transport.AdbSocketSource
import com.celmonitor.transport.ConnectionSource
import java.security.MessageDigest

/** App-wide owner of the listening transport and the active session. UI callbacks arrive on the main thread. */
class ConnectionManager(private val context: Context) {

    sealed class State {
        data class Waiting(val lastError: String?) : State()
        data class Connected(val hostName: String, val transport: String) : State()
        data class Streaming(val hostName: String, val transport: String, val config: StreamConfig) : State()
        object Paused : State()
    }

    interface Listener {
        fun onState(state: State)
        fun onHostStatus(status: HostStatus) {}
        fun onNotice(notice: Notice) {}
    }

    private val main = Handler(Looper.getMainLooper())
    private var source: ConnectionSource? = null
    private var acceptThread: Thread? = null
    @Volatile private var session: ClientSession? = null
    @Volatile private var surface: Surface? = null

    var listener: Listener? = null
        set(value) { field = value; value?.onState(state) }

    var state: State = State.Waiting(null); private set
    var decoderName = ""; private set
    var decoderLowLatency = false; private set
    var hostStatus: HostStatus? = null; private set

    val stats get() = session?.stats

    @Volatile private var paused = false

    fun start() {
        paused = false
        if (acceptThread != null) return
        val src = AdbSocketSource()
        source = src
        setState(State.Waiting((state as? State.Waiting)?.lastError))
        acceptThread = Thread({ acceptLoop(src) }, "celmon-accept").apply { start() }
    }

    /** Leaves monitor mode: ends the session and stops listening until [start] is called again. */
    fun pause() {
        paused = true
        stopInternal()
        setState(State.Paused)
    }

    private fun stopInternal() {
        source?.close()
        source = null
        session?.close(DisconnectReason.NORMAL, "usuário saiu do modo monitor")
        acceptThread = null
    }

    private fun acceptLoop(src: ConnectionSource) {
        while (true) {
            val conn = src.accept() ?: break
            Log.i(TAG, "connected via ${conn.description}")
            val hello = try { buildHello() } catch (e: Exception) {
                Log.e(TAG, "cannot describe display", e)
                conn.close()
                setState(State.Waiting("não foi possível ler a tela/decoders: ${e.message}"))
                continue
            }
            var hostName = "PC"
            val s = ClientSession(conn, hello, object : ClientSession.Events {
                override fun onHandshake(ack: HelloAck) {
                    hostName = ack.hostName.ifBlank { "PC" }
                    setState(State.Connected(hostName, conn.description))
                }
                override fun onStreamConfig(config: StreamConfig) = setState(State.Streaming(hostName, conn.description, config))
                override fun onHostStatus(status: HostStatus) { hostStatus = status; main.post { listener?.onHostStatus(status) } }
                override fun onNotice(notice: Notice) { main.post { listener?.onNotice(notice) } }
                override fun onDecoderInfo(name: String, lowLatency: Boolean) { decoderName = name; decoderLowLatency = lowLatency }
                override fun onEnded(reason: Int, message: String, remote: Boolean) {
                    Log.i(TAG, "session ended reason=$reason remote=$remote msg=$message")
                    if (!paused) {
                        setState(State.Waiting(if (reason == DisconnectReason.NORMAL) null else message.ifBlank { null }))
                    }
                }
            })
            surface?.let { s.attachSurface(it) }
            session = s
            s.run() // blocks until the session ends
            session = null
            hostStatus = null
        }
        Log.i(TAG, "accept loop finished")
    }

    fun attachSurface(s: Surface) { surface = s; session?.attachSurface(s) }
    fun detachSurface() { surface = null; session?.detachSurface() }

    fun sendMouse(m: InputMouse) { session?.sendMouse(m) }
    fun sendTouch(c: List<TouchContact>) { session?.sendTouch(c) }
    fun sendSettings(s: ClientSettings) { session?.sendSettings(s) }

    private fun setState(s: State) {
        state = s
        main.post { listener?.onState(s) }
    }

    private fun buildHello(): Hello {
        val info = DeviceDisplay.query(context)
        if (info.modes.isEmpty()) throw IllegalStateException("nenhum decoder de vídeo compatível")
        var caps = 0
        for (c in info.codecs) caps = caps or c.capBit
        if (info.lowLatencyDecoder) caps = caps or Proto.CAP_LOW_LATENCY_DECODER
        caps = caps or Proto.CAP_TOUCH
        return Hello(
            capabilities = caps,
            screenWidth = info.widthPx,
            screenHeight = info.heightPx,
            densityDpi = info.densityDpi,
            refreshRateMilliHz = info.refreshRateMilliHz,
            rotation = info.rotation,
            modes = info.modes,
            deviceId = deviceId(),
            manufacturer = Build.MANUFACTURER,
            model = Build.MODEL,
            androidVersion = Build.VERSION.RELEASE,
            appVersion = BuildConfig.VERSION_NAME,
        )
    }

    /** Stable per phone (and per signing key), never the raw ANDROID_ID. */
    @SuppressLint("HardwareIds")
    private fun deviceId(): String {
        val raw = Settings.Secure.getString(context.contentResolver, Settings.Secure.ANDROID_ID) ?: Build.MODEL
        val d = MessageDigest.getInstance("SHA-256").digest(("celmonitor:$raw").toByteArray())
        return d.take(8).joinToString("") { "%02x".format(it) }
    }

    companion object {
        private const val TAG = "ConnectionManager"
    }
}
