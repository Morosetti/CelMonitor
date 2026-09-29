package com.celmonitor.session

import android.os.SystemClock
import android.util.Log
import android.view.Surface
import com.celmonitor.decoder.VideoDecoder
import com.celmonitor.protocol.ClientSettings
import com.celmonitor.protocol.Codecs
import com.celmonitor.protocol.Disconnect
import com.celmonitor.protocol.DisconnectReason
import com.celmonitor.protocol.FrameAck
import com.celmonitor.protocol.Heartbeat
import com.celmonitor.protocol.Hello
import com.celmonitor.protocol.HelloAck
import com.celmonitor.protocol.HostStatus
import com.celmonitor.protocol.InputMouse
import com.celmonitor.protocol.KeyframeReason
import com.celmonitor.protocol.Message
import com.celmonitor.protocol.MessageChannel
import com.celmonitor.protocol.MsgType
import com.celmonitor.protocol.Notice
import com.celmonitor.protocol.Proto
import com.celmonitor.protocol.ProtocolException
import com.celmonitor.protocol.StreamConfig
import com.celmonitor.protocol.TouchContact
import com.celmonitor.protocol.VideoFrameHeader
import com.celmonitor.transport.Connection
import java.io.EOFException
import java.io.IOException
import java.util.concurrent.Executors
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * One connection to the Windows host: handshake, stream configuration, video, heartbeats, input.
 * Threads: reader (this.run), writer (outbound queue), heartbeat; decoder callbacks only enqueue.
 */
class ClientSession(
    private val connection: Connection,
    private val hello: Hello,
    private val events: Events,
) : VideoDecoder.Listener {

    /** All callbacks come from background threads; the UI layer must hop to the main thread. */
    interface Events {
        fun onHandshake(ack: HelloAck)
        fun onStreamConfig(config: StreamConfig)
        fun onHostStatus(status: HostStatus)
        fun onNotice(notice: Notice)
        fun onDecoderInfo(name: String, lowLatency: Boolean)
        fun onEnded(reason: Int, message: String, remote: Boolean)
    }

    data class Stats(val receivedFps: Float, val receivedKbps: Int, val renderUs: Int, val rttUs: Int)

    private class Outbound(val type: Int, val payload: ByteArray)

    private val channel = MessageChannel(connection.input, connection.output)
    private val outbox = LinkedBlockingQueue<Outbound>(256)
    private val lock = Any()

    @Volatile private var running = true
    @Volatile private var lastReceivedMs = SystemClock.elapsedRealtime()
    @Volatile private var rttUs = 0
    private var heartbeatCounter = 0

    private var config: StreamConfig? = null
    private var surface: Surface? = null
    private var decoder: VideoDecoder? = null
    private var generation = 0 // bumps on every config/surface change; stale decoder starts are discarded
    private val decoderExecutor = Executors.newSingleThreadExecutor { Thread(it, "celmon-decoder-setup") }

    // Stats (reader thread writes, UI reads a snapshot).
    private var statWindowStart = SystemClock.elapsedRealtime()
    private var statFrames = 0
    private var statBytes = 0L
    @Volatile private var lastRenderUs = 0
    @Volatile var stats = Stats(0f, 0, 0, 0); private set

    private var writerThread: Thread? = null
    private var heartbeatThread: Thread? = null

    /** Blocking: runs the session on the calling thread until the connection ends. */
    fun run() {
        writerThread = Thread(::writerLoop, "celmon-writer").apply { start() }
        heartbeatThread = Thread(::heartbeatLoop, "celmon-heartbeat").apply { start() }
        var reason = DisconnectReason.NORMAL
        var message = ""
        var remote = false
        try {
            enqueue(MsgType.HELLO, Codecs.encode(hello))
            while (running) {
                val msg = channel.read()
                messagesRead++
                lastReceivedMs = SystemClock.elapsedRealtime()
                if (handle(msg)) { remote = true; reason = lastRemoteReason; message = lastRemoteMessage; break }
            }
        } catch (e: ProtocolException) {
            Log.e(TAG, "protocol error", e)
            reason = DisconnectReason.PROTOCOL_ERROR; message = e.message ?: "erro de protocolo"
            sendDirect(MsgType.DISCONNECT, Codecs.encode(Disconnect(reason, message)))
        } catch (e: IOException) {
            if (localEnd == null) Log.w(TAG, "connection lost via ${connection.description} after $messagesRead messages", e)
            val local = localEnd
            if (local != null) { reason = local.first; message = local.second }
            else { reason = DisconnectReason.TIMEOUT; message = if (e is EOFException) "conexão encerrada pelo PC" else "conexão perdida: ${e.message}" }
        } finally {
            shutdown()
            events.onEnded(reason, message, remote)
        }
    }

    private var messagesRead = 0L

    /** Set when this side ends the session, so the reader reports the right reason. */
    @Volatile private var localEnd: Pair<Int, String>? = null

    private fun endLocally(reason: Int, message: String) {
        if (localEnd != null) return
        localEnd = reason to message
        if (reason != DisconnectReason.TIMEOUT) sendDirect(MsgType.DISCONNECT, Codecs.encode(Disconnect(reason, message)))
        running = false
        connection.close() // unblocks the reader, which reports the end
    }

    private var handshakeDone = false
    private var lastRemoteReason = 0
    private var lastRemoteMessage = ""

    /** @return true if the host asked to disconnect. */
    private fun handle(msg: Message): Boolean {
        val p = msg.payload
        // HEARTBEAT is tolerated early: with USB accessory a heartbeat of a previous PC attempt can still sit in the
        // USB buffer when the app opens the accessory.
        if (!handshakeDone && msg.type != MsgType.HELLO_ACK && msg.type != MsgType.DISCONNECT && msg.type != MsgType.NOTICE &&
            msg.type != MsgType.HEARTBEAT)
            throw ProtocolException("message ${msg.type} before handshake")
        when (msg.type) {
            MsgType.HELLO_ACK -> {
                if (handshakeDone) throw ProtocolException("duplicate HELLO_ACK")
                val ack = Codecs.parseHelloAck(p)
                if (ack.versionMajor != Proto.VERSION_MAJOR) throw ProtocolException("host protocol ${ack.versionMajor}")
                handshakeDone = true
                events.onHandshake(ack)
            }
            MsgType.STREAM_CONFIG -> {
                val cfg = Codecs.parseStreamConfig(p)
                if (cfg.codec !in supportedCodecs()) {
                    enqueue(MsgType.STREAM_READY, Codecs.encodeStreamReady(cfg.streamId, 1))
                    return false
                }
                synchronized(lock) { config = cfg }
                scheduleDecoder(KeyframeReason.DECODER_CREATED, announce = true)
                events.onStreamConfig(cfg)
            }
            MsgType.VIDEO_FRAME -> onVideoFrame(msg)
            MsgType.HEARTBEAT -> {
                // echo == 0: a request, answered immediately; echo != 0: the answer to ours -> round-trip time.
                val hb = Codecs.parseHeartbeat(p)
                if (hb.echoTimeUs == 0L) enqueue(MsgType.HEARTBEAT, Codecs.encode(Heartbeat(nowUs(), hb.senderTimeUs, heartbeatCounter++)))
                else rttUs = (nowUs() - hb.echoTimeUs).toInt().coerceAtLeast(0)
            }
            MsgType.HOST_STATUS -> events.onHostStatus(Codecs.parseHostStatus(p))
            MsgType.NOTICE -> events.onNotice(Codecs.parseNotice(p))
            MsgType.DISCONNECT -> {
                val d = Codecs.parseDisconnect(p)
                lastRemoteReason = d.reason; lastRemoteMessage = d.message
                return true
            }
            // Host-bound messages arriving here are a protocol violation.
            else -> throw ProtocolException("unexpected message ${msg.type}")
        }
        return false
    }

    private fun onVideoFrame(msg: Message) {
        val h = Codecs.parseVideoFrameHeader(msg.payload, msg.length)
        statFrames += if (h.isCodecConfig) 0 else 1
        statBytes += msg.length
        updateStats()
        val dec: VideoDecoder?
        synchronized(lock) {
            if (h.streamId != config?.streamId) return // stale stream
            dec = decoder
        }
        if (dec == null) {
            // No surface right now (screen off / app in background): keep the host flowing.
            if (!h.isCodecConfig) ack(h, 0, 0)
            return
        }
        dec.submit(h, msg.payload, VideoFrameHeader.SIZE, msg.length - VideoFrameHeader.SIZE)
    }

    private fun updateStats() {
        val now = SystemClock.elapsedRealtime()
        val dt = now - statWindowStart
        if (dt < 1000) return
        stats = Stats(statFrames * 1000f / dt, (statBytes * 8 / dt).toInt(), lastRenderUs, rttUs)
        statFrames = 0; statBytes = 0; statWindowStart = now
    }

    // ------------------------------------------------------------------ surface / decoder

    fun attachSurface(s: Surface) {
        synchronized(lock) {
            surface = s
            if (config == null) return
        }
        scheduleDecoder(KeyframeReason.SURFACE_RECREATED, announce = false)
    }

    /** Must release synchronously: the surface is gone when surfaceDestroyed() returns. */
    fun detachSurface() {
        val old: VideoDecoder?
        synchronized(lock) {
            surface = null
            generation++
            old = decoder
            decoder = null
        }
        old?.release()
    }

    /**
     * (Re)creates the decoder on a dedicated thread. Creating a hardware decoder can take seconds (measured ~4 s for
     * OMX.qcom on a Mi Max 3), so it must never run on the UI thread nor while holding [lock] — that froze the UI
     * and stopped the network reader. Frames arriving meanwhile are ACKed and dropped; a keyframe is requested after.
     */
    private fun scheduleDecoder(reason: Int, announce: Boolean) {
        val gen: Int
        val cfg: StreamConfig?
        val s: Surface?
        val old: VideoDecoder?
        synchronized(lock) {
            gen = ++generation
            cfg = config
            s = surface
            old = decoder
            decoder = null
        }
        if (!running || decoderExecutor.isShutdown) { old?.release(); return }
        decoderExecutor.execute {
            old?.release()
            if (cfg == null || !running) return@execute
            if (s == null || !s.isValid) {
                // No surface yet (screen off / app in background): accept the stream, decode once visible.
                if (announce) enqueue(MsgType.STREAM_READY, Codecs.encodeStreamReady(cfg.streamId, 0))
                return@execute
            }
            val d = VideoDecoder(cfg, s, this)
            try {
                d.start()
            } catch (e: Exception) {
                Log.e(TAG, "decoder start failed", e)
                d.release()
                if (!s.isValid) return@execute  // surface vanished while starting; will retry on attach
                if (announce) sendDirect(MsgType.STREAM_READY, Codecs.encodeStreamReady(cfg.streamId, 2))
                endLocally(DisconnectReason.DECODER_ERROR, "decoder de vídeo indisponível: ${e.message}")
                return@execute
            }
            val current = synchronized(lock) {
                (gen == generation && running).also { if (it) decoder = d }
            }
            if (!current) { d.release(); return@execute }
            events.onDecoderInfo(d.name, d.lowLatencyEnabled)
            if (announce) enqueue(MsgType.STREAM_READY, Codecs.encodeStreamReady(cfg.streamId, 0))
            else enqueue(MsgType.REQUEST_KEYFRAME, Codecs.encodeRequestKeyframe(cfg.streamId, reason))
        }
    }

    override fun onFrameQueued(frame: VideoFrameHeader, processingUs: Int) = ack(frame, processingUs, 0)

    override fun onFrameRendered(frame: VideoFrameHeader, processingUs: Int) {
        lastRenderUs = processingUs
        ack(frame, processingUs, Proto.ACK_RENDERED)
    }

    override fun onDecoderError(message: String) {
        Log.w(TAG, "decoder error: $message — recreating")
        // Runs on the decoder executor, never on the codec's own callback thread (releasing there deadlocks).
        if (running) scheduleDecoder(KeyframeReason.DECODE_ERROR, announce = false)
    }

    private fun ack(h: VideoFrameHeader, processingUs: Int, flags: Int) =
        enqueue(MsgType.FRAME_ACK, Codecs.encode(FrameAck(h.streamId, h.frameNumber, h.captureTimeUs, processingUs, flags)))

    private fun supportedCodecs() = com.celmonitor.protocol.Codec.entries.filter { hello.capabilities and it.capBit != 0 }

    // ------------------------------------------------------------------ outbound API (any thread)

    fun sendMouse(m: InputMouse) = enqueue(MsgType.INPUT_MOUSE, Codecs.encode(m))
    fun sendTouch(contacts: List<TouchContact>) = enqueue(MsgType.INPUT_TOUCH, Codecs.encodeTouch(contacts))
    fun sendSettings(s: ClientSettings) = enqueue(MsgType.CLIENT_SETTINGS, Codecs.encode(s))

    fun close(reason: Int = DisconnectReason.NORMAL, message: String = "") = endLocally(reason, message)

    private fun enqueue(type: Int, payload: ByteArray) {
        if (!running) return
        if (!outbox.offer(Outbound(type, payload))) {
            Log.w(TAG, "outbox full, dropping message $type")
        }
    }

    private fun sendDirect(type: Int, payload: ByteArray) {
        runCatching { channel.send(type, payload) }
    }

    private fun writerLoop() {
        try {
            while (running) {
                val m = outbox.poll(200, TimeUnit.MILLISECONDS) ?: continue
                channel.send(m.type, m.payload)
            }
        } catch (e: IOException) {
            if (running) { Log.w(TAG, "write failed", e); endLocally(DisconnectReason.TIMEOUT, "falha ao enviar: ${e.message}") }
        } catch (_: InterruptedException) {
        }
    }

    private fun heartbeatLoop() {
        try {
            while (running) {
                Thread.sleep(HEARTBEAT_INTERVAL_MS)
                enqueue(MsgType.HEARTBEAT, Codecs.encode(Heartbeat(nowUs(), 0, heartbeatCounter++)))
                if (SystemClock.elapsedRealtime() - lastReceivedMs > TIMEOUT_MS) {
                    Log.w(TAG, "host timeout")
                    endLocally(DisconnectReason.TIMEOUT, "o PC parou de responder")
                }
            }
        } catch (_: InterruptedException) {
        }
    }

    private fun shutdown() {
        running = false
        decoderExecutor.shutdown()
        val d = synchronized(lock) { generation++; decoder.also { decoder = null } }
        d?.release()
        writerThread?.interrupt()
        heartbeatThread?.interrupt()
        connection.close()
    }

    private fun nowUs() = SystemClock.elapsedRealtimeNanos() / 1000

    companion object {
        private const val TAG = "ClientSession"
        const val HEARTBEAT_INTERVAL_MS = 1000L
        const val TIMEOUT_MS = 5000L
    }
}
