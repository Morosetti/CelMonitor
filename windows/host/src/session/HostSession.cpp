#include "HostSession.h"

#include <algorithm>
#include <cstring>

#include "../util/Log.h"

namespace celmon {

using namespace proto;

namespace {

constexpr uint32_t kFlowWindow = 3;               // frames handed to the phone's decoder queue but not yet ACKed
constexpr uint64_t kHeartbeatUs = 1000000;
constexpr uint64_t kTimeoutUs = 5000000;
constexpr size_t kMaxQueuedBytes = 64u << 20;     // a stuck link, not normal operation

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

uint64_t Fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

}  // namespace

const wchar_t* DisconnectText(DisconnectReason r) {
    switch (r) {
    case DisconnectReason::Normal: return L"desconectado pelo usuário";
    case DisconnectReason::ProtocolError: return L"erro de protocolo";
    case DisconnectReason::VersionMismatch: return L"versões incompatíveis do app e do programa do PC";
    case DisconnectReason::Timeout: return L"conexão perdida";
    case DisconnectReason::DisplayError: return L"falha no monitor virtual";
    case DisconnectReason::EncoderError: return L"falha no encoder de vídeo";
    case DisconnectReason::DecoderError: return L"o celular não conseguiu decodificar o vídeo";
    case DisconnectReason::Shutdown: return L"encerrado";
    }
    return L"desconectado";
}

HostSession::HostSession(std::unique_ptr<IConnection> conn, SessionSettings settings, Events events)
    : conn_(std::move(conn)), settings_(settings), events_(std::move(events)) {
    info_.transport = conn_->Description();
}

HostSession::~HostSession() {
    Stop(L"");
    if (reader_.joinable()) reader_.join();
    if (writer_.joinable()) writer_.join();
}

void HostSession::Start() {
    lastReceivedUs_ = NowUs();
    writer_ = std::thread(&HostSession::WriterLoop, this);
    reader_ = std::thread(&HostSession::ReaderLoop, this);
}

void HostSession::Stop(const std::wstring& reason) {
    End(DisconnectReason::Normal, reason.empty() ? L"desconectado pelo usuário" : reason, true);
}

SessionInfo HostSession::Info() const {
    std::lock_guard<std::mutex> lock(lock_);
    return info_;
}

// ------------------------------------------------------------------------------------------------ sending

void HostSession::Send(MsgType type, const std::vector<uint8_t>& payload, uint16_t flags) {
    std::vector<uint8_t> msg(kHeaderSize + payload.size());
    Header h;
    h.type = uint16_t(type);
    h.flags = flags;
    h.length = uint32_t(payload.size());
    {
        std::lock_guard<std::mutex> lock(queueLock_);
        h.seq = sendSeq_++;
    }
    encodeHeader(msg.data(), h);
    if (!payload.empty()) memcpy(msg.data() + kHeaderSize, payload.data(), payload.size());
    SendRaw(std::move(msg));
}

void HostSession::SendRaw(std::vector<uint8_t>&& message) {
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(queueLock_);
        if (ended_ && !stopping_) return;
        queuedBytes_ += message.size();
        overflow = queuedBytes_ > kMaxQueuedBytes;
        if (!overflow) queue_.push_back(std::move(message));
    }
    queueCv_.notify_one();
    if (overflow) End(DisconnectReason::Timeout, L"o celular não está recebendo dados (fila de envio cheia)", false);
}

void HostSession::WriterLoop() {
    uint64_t nextBeat = NowUs();
    uint32_t counter = 0;
    statStartUs_ = NowUs();
    for (;;) {
        std::vector<uint8_t> msg;
        {
            std::unique_lock<std::mutex> lock(queueLock_);
            queueCv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return !queue_.empty() || ended_; });
            if (!queue_.empty()) {
                msg = std::move(queue_.front());
                queue_.pop_front();
                queuedBytes_ -= msg.size();
            } else if (ended_) {
                break;
            }
        }
        if (!msg.empty() && !conn_->WriteAll(msg.data(), msg.size())) {
            End(DisconnectReason::Timeout, L"falha ao enviar dados ao celular (cabo desconectado?)", false);
            break;
        }
        uint64_t now = NowUs();
        if (now >= nextBeat && !ended_) {
            nextBeat = now + kHeartbeatUs;
            Heartbeat hb;
            hb.senderTimeUs = now;
            hb.echoTimeUs = 0;  // request: the phone answers immediately
            hb.counter = counter++;
            Send(MsgType::Heartbeat, serialize(hb));

            HostStatus st;
            {
                std::lock_guard<std::mutex> lock(lock_);
                double secs = (now - statStartUs_) / 1e6;
                info_.fps = float(statFrames_ / secs);
                info_.kbps = uint32_t(statBytes_ * 8 / secs / 1000);
                statFrames_ = statBytes_ = 0;
                statStartUs_ = now;
                info_.encodeUs = pipeline_.Stats().lastEncodeUs;
                st.encodeFpsX100 = uint32_t(info_.fps * 100);
                st.bitrateKbps = info_.kbps;
                st.latencyUs = info_.latencyUs;
                st.encoderName = Utf8(info_.encoder);
            }
            Send(MsgType::HostStatus, serialize(st), kFlagIgnorable);
            Changed();
            if (now - lastReceivedUs_ > kTimeoutUs) {
                End(DisconnectReason::Timeout, L"o celular parou de responder", false);
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------ receiving

void HostSession::ReaderLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<uint8_t> payload;
    uint8_t hdr[kHeaderSize];
    while (!ended_) {
        if (!conn_->ReadExact(hdr, kHeaderSize)) {
            End(DisconnectReason::Timeout, L"o celular desconectou (cabo removido ou app fechado)", false);
            break;
        }
        Header h;
        HeaderStatus hs = decodeHeader(hdr, h);
        if (hs == HeaderStatus::BadMagic || hs == HeaderStatus::TooLarge || hs == HeaderStatus::UnknownFatal) {
            End(DisconnectReason::ProtocolError, L"dados inválidos recebidos do celular", true);
            break;
        }
        payload.resize(h.length);
        if (h.length && !conn_->ReadExact(payload.data(), h.length)) {
            End(DisconnectReason::Timeout, L"o celular desconectou", false);
            break;
        }
        lastReceivedUs_ = NowUs();
        if (hs == HeaderStatus::UnknownIgnorable) continue;
        Handle(h.type, payload);
        lastReceivedUs_ = NowUs();  // OnHello can take seconds (monitor creation)
    }

    // Teardown happens here, on the thread that owns the session, never inside a callback.
    pipeline_.Stop();
    Status s = display_.Remove();
    if (!s.ok && events_.onWarning) events_.onWarning(s);
    {
        std::lock_guard<std::mutex> lock(lock_);
        info_.state = SessionState::Ended;
    }
    queueCv_.notify_all();
    if (writer_.joinable() && writer_.get_id() != std::this_thread::get_id()) writer_.join();
    conn_->Close();
    std::wstring why;
    bool byPhoneUser;
    {
        std::lock_guard<std::mutex> lock(lock_);
        why = endReason_;
        byPhoneUser = endedByPhoneUser_;
    }
    Changed();
    if (events_.onEnded) events_.onEnded(why, byPhoneUser);
    CoUninitialize();
}

void HostSession::End(DisconnectReason reason, const std::wstring& why, bool notifyPeer) {
    if (ended_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(lock_);
        endReason_ = why;
    }
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, why.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
    Log::Info("session ending (%d): %s", int(reason), buf);
    if (notifyPeer) {
        // Best effort: write DISCONNECT directly so it goes out before the socket closes.
        Disconnect d;
        d.reason = reason;
        d.message = Utf8(why);
        auto p = serialize(d);
        std::vector<uint8_t> msg(kHeaderSize + p.size());
        Header h;
        h.type = uint16_t(MsgType::Disconnect);
        h.length = uint32_t(p.size());
        encodeHeader(msg.data(), h);
        memcpy(msg.data() + kHeaderSize, p.data(), p.size());
        {
            std::lock_guard<std::mutex> lock(queueLock_);
            stopping_ = true;
            queue_.push_back(std::move(msg));
        }
        queueCv_.notify_one();
        // Give the writer a moment to flush it, then unblock the reader.
        for (int i = 0; i < 20; ++i) {
            {
                std::lock_guard<std::mutex> lock(queueLock_);
                if (queue_.empty()) break;
            }
            Sleep(10);
        }
    }
    conn_->Close();
    queueCv_.notify_all();
}

void HostSession::Handle(uint16_t type, const std::vector<uint8_t>& p) {
    const uint8_t* d = p.data();
    size_t n = p.size();
    auto bad = [&](const wchar_t* what) {
        End(DisconnectReason::ProtocolError, std::wstring(L"mensagem inválida do celular: ") + what, true);
    };
    bool handshaken;
    {
        std::lock_guard<std::mutex> lock(lock_);
        handshaken = !hello_.deviceId.empty();
    }
    if (!handshaken && MsgType(type) != MsgType::Hello && MsgType(type) != MsgType::Disconnect &&
        MsgType(type) != MsgType::Heartbeat)
        return bad(L"mensagem antes do HELLO");

    switch (MsgType(type)) {
    case MsgType::Hello: {
        if (handshaken) return bad(L"HELLO repetido");
        auto h = parseHello(d, n);
        if (!h) return bad(L"HELLO");
        OnHello(*h);
        break;
    }
    case MsgType::StreamReady: {
        auto r = parseStreamReady(d, n);
        if (!r) return bad(L"STREAM_READY");
        {
            std::lock_guard<std::mutex> lock(lock_);
            if (r->streamId != streamId_) break;
            if (r->status == 0) {
                streamReady_ = true;
                info_.state = SessionState::Streaming;
            }
        }
        if (r->status != 0) {
            End(DisconnectReason::DecoderError, L"o celular não conseguiu iniciar o decoder de vídeo para este modo", true);
            break;
        }
        pipeline_.RequestKeyframe();
        Changed();
        break;
    }
    case MsgType::FrameAck: {
        auto a = parseFrameAck(d, n);
        if (!a) return bad(L"FRAME_ACK");
        std::lock_guard<std::mutex> lock(lock_);
        if (a->streamId != streamId_) break;
        if (a->flags & FrameAck::Rendered) {
            uint64_t now = NowUs();
            if (a->captureTimeUs && a->captureTimeUs <= now) {
                uint32_t lat = uint32_t(now - a->captureTimeUs);
                info_.latencyUs = info_.latencyUs ? (info_.latencyUs * 7 + lat) / 8 : lat;
            }
        } else if (a->frameNumber > lastFrameAcked_ && a->frameNumber <= lastFrameSent_) {
            lastFrameAcked_ = a->frameNumber;
        }
        break;
    }
    case MsgType::RequestKeyframe: {
        if (!parseRequestKeyframe(d, n)) return bad(L"REQUEST_KEYFRAME");
        pipeline_.RequestKeyframe();
        break;
    }
    case MsgType::Heartbeat: {
        // echo == 0: the peer's periodic request, answered at once; echo != 0: the answer to ours -> RTT.
        auto hb = parseHeartbeat(d, n);
        if (!hb) return bad(L"HEARTBEAT");
        if (hb->echoTimeUs == 0) {
            Heartbeat reply;
            reply.senderTimeUs = NowUs();
            reply.echoTimeUs = hb->senderTimeUs;
            Send(MsgType::Heartbeat, serialize(reply));
        } else {
            uint64_t now = NowUs();
            if (hb->echoTimeUs <= now) {
                std::lock_guard<std::mutex> lock(lock_);
                info_.rttUs = uint32_t(now - hb->echoTimeUs);
            }
        }
        break;
    }
    case MsgType::ClientSettings: {
        auto s = parseClientSettings(d, n);
        if (!s) return bad(L"CLIENT_SETTINGS");
        if (s->mask & ClientSettings::MaskFps) SetFps(s->fps);
        if (s->mask & ClientSettings::MaskQuality) SetQuality(s->quality);
        if (s->mask & ClientSettings::MaskOrientation) SetOrientation(s->orientation == 1);
        break;
    }
    case MsgType::InputMouse:
        if (!parseInputMouse(d, n)) return bad(L"INPUT_MOUSE");
        break;  // phase 2
    case MsgType::InputTouch:
        if (!parseInputTouch(d, n)) return bad(L"INPUT_TOUCH");
        break;  // phase 2
    case MsgType::Disconnect: {
        auto m = parseDisconnect(d, n);
        if (m && m->reason == DisconnectReason::Normal) {
            std::lock_guard<std::mutex> lock(lock_);
            endedByPhoneUser_ = true;
        }
        std::wstring why = m && !m->message.empty() ? Wide(m->message) : DisconnectText(m ? m->reason : DisconnectReason::Normal);
        End(m ? m->reason : DisconnectReason::Normal, L"celular: " + why, false);
        break;
    }
    default:
        // Host-bound types only; anything else from the phone is a protocol violation.
        return bad(L"tipo inesperado");
    }
}

// ------------------------------------------------------------------------------------------------ handshake

uint32_t HostSession::BitrateFor(uint32_t w, uint32_t h) const {
    double factor = 0.25 + 1.5 * std::clamp<uint32_t>(settings_.quality, 1, 100) / 100.0;  // q50 -> 1.0x
    return uint32_t(VideoPipeline::DefaultBitrateKbps(w, h, settings_.fps) * factor);
}

void HostSession::OnHello(const Hello& h) {
    if (h.versionMajor != kVersionMajor) {
        End(DisconnectReason::VersionMismatch, L"o app do celular usa outra versão do protocolo; atualize os dois lados", true);
        return;
    }
    uint16_t codecBit = uint16_t(1u << (uint8_t(settings_.codec) - 1));
    std::vector<Mode> modes;
    for (auto& m : h.modes)
        if (m.codecMask & codecBit) modes.push_back({m.width, m.height, std::min<uint32_t>(m.maxFps, 60)});
    if (modes.empty() && settings_.codec != Codec::H264) {
        if (events_.onWarning) events_.onWarning(Status::Error(L"O celular não decodifica o codec escolhido; usando H.264."));
        settings_.codec = Codec::H264;
        for (auto& m : h.modes)
            if (m.codecMask & 1) modes.push_back({m.width, m.height, std::min<uint32_t>(m.maxFps, 60)});
    }
    if (modes.empty()) {
        End(DisconnectReason::DecoderError, L"o celular não tem decoder de vídeo compatível", true);
        return;
    }

    // Initial mode: explicit choice if supported, else the phone's native panel in the requested orientation.
    size_t preferred = 0;
    uint64_t bestArea = 0;
    for (size_t i = 0; i < modes.size(); ++i) {
        bool portrait = modes[i].height > modes[i].width;
        uint64_t area = uint64_t(modes[i].width) * modes[i].height;
        if (portrait == settings_.portrait && area > bestArea) { bestArea = area; preferred = i; }
    }
    if (settings_.mode) {
        for (size_t i = 0; i < modes.size(); ++i)
            if (modes[i].width == settings_.mode->width && modes[i].height == settings_.mode->height) preferred = i;
    }

    deviceKey_ = Fnv1a(h.deviceId);
    std::string name;
    for (char c : h.model) if (c >= 0x20 && c < 0x7F && name.size() < 13) name += c;
    if (name.empty()) name = "CelMonitor";
    {
        std::lock_guard<std::mutex> lock(lock_);
        hello_ = h;
        info_.manufacturer = Wide(h.manufacturer);
        info_.model = Wide(h.model);
        info_.androidVersion = Wide(h.androidVersion);
        info_.phoneWidth = h.screenWidth;
        info_.phoneHeight = h.screenHeight;
        info_.supportedModes = modes;
        info_.state = SessionState::CreatingMonitor;
    }
    Log::Info("phone: %s %s (Android %s), screen %ux%u, %zu usable modes", h.manufacturer.c_str(), h.model.c_str(),
              h.androidVersion.c_str(), h.screenWidth, h.screenHeight, modes.size());
    Changed();

    HelloAck ack;
    ack.sessionId = uint32_t(NowUs());
    wchar_t host[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = ARRAYSIZE(host);
    ack.hostName = GetComputerNameW(host, &len) ? Utf8(host) : "PC";
    Send(MsgType::HelloAck, serialize(ack));

    Status s = display_.Add(deviceKey_, name, modes, preferred);
    if (!s.ok) {
        Notice nt;
        nt.severity = 2;
        nt.message = Utf8(s.message);
        Send(MsgType::Notice, serialize(nt), kFlagIgnorable);
        End(DisconnectReason::DisplayError, s.message, true);
        return;
    }
    std::wstring gdi;
    for (int i = 0; i < 50 && gdi.empty() && !ended_; ++i) {
        Sleep(100);
        gdi = display_.GdiDeviceName();
    }
    if (gdi.empty()) {
        End(DisconnectReason::DisplayError,
            L"o Windows não ativou o monitor virtual (verifique Configurações > Tela: ele pode estar desativado)", true);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(lock_);
        info_.displayName = gdi;
    }

    auto current = display_.CurrentMode().value_or(modes[preferred]);
    PipelineConfig pc;
    pc.gdiName = gdi;
    pc.codec = settings_.codec;
    pc.fps = settings_.fps;
    pc.bitrateKbps = BitrateFor(current.width, current.height);
    PipelineEvents pe;
    pe.onStreamStart = [this](uint32_t w, uint32_t hh, const std::wstring& enc) { OnStreamStart(w, hh, enc); };
    pe.onFrame = [this](EncodedFrame&& f) { OnFrame(std::move(f)); };
    pe.onError = [this](const Status& e) {
        if (events_.onWarning) events_.onWarning(e);
        Notice nt;
        nt.severity = 1;
        nt.message = Utf8(e.message);
        Send(MsgType::Notice, serialize(nt), kFlagIgnorable);
    };
    pe.canSend = [this] {
        std::lock_guard<std::mutex> lock(lock_);
        return streamReady_ && lastFrameSent_ - lastFrameAcked_ < kFlowWindow;
    };
    pipeline_.Start(pc, pe);
}

void HostSession::OnStreamStart(uint32_t w, uint32_t h, const std::wstring& encoder) {
    StreamConfig c;
    {
        std::lock_guard<std::mutex> lock(lock_);
        c.streamId = ++streamId_;
        streamReady_ = false;
        lastFrameSent_ = lastFrameAcked_ = 0;
        info_.mode = {w, h, settings_.fps};
        info_.encoder = encoder;
        info_.state = SessionState::CreatingMonitor;
    }
    c.width = w;
    c.height = h;
    c.fps = uint16_t(settings_.fps);
    c.codec = settings_.codec;
    c.orientation = h > w ? 1 : 0;
    c.bitrateKbps = BitrateFor(w, h);
    pipeline_.SetBitrate(c.bitrateKbps);
    Send(MsgType::StreamConfig, serialize(c));
    Changed();
}

void HostSession::OnFrame(EncodedFrame&& f) {
    uint32_t streamId, frameNo;
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (!streamReady_) return;  // phone not ready; a keyframe is requested once it is
        streamId = streamId_;
        frameNo = ++lastFrameSent_;
        statFrames_++;
        statBytes_ += f.config.size() + f.data.size();
    }
    auto sendFrame = [&](const std::vector<uint8_t>& data, uint16_t flags, uint32_t number) {
        Writer w;
        VideoFrameHeader vh;
        vh.streamId = streamId;
        vh.frameNumber = number;
        vh.captureTimeUs = f.captureTimeUs;
        vh.ptsUs = f.ptsUs;
        vh.flags = flags;
        serializeVideoFrameHeader(w, vh);
        w.bytes(data.data(), data.size());
        Send(MsgType::VideoFrame, w.data());
    };
    if (!f.config.empty()) sendFrame(f.config, FrameCodecConfig, frameNo);
    sendFrame(f.data, f.key ? FrameKey : 0, frameNo);
}

// ------------------------------------------------------------------------------------------------ live settings

void HostSession::SetFps(uint32_t fps) {
    fps = std::clamp<uint32_t>(fps, 1, 60);
    settings_.fps = fps;
    pipeline_.SetFps(fps);
    auto m = Info().mode;
    if (m.width) pipeline_.SetBitrate(BitrateFor(m.width, m.height));
}

void HostSession::SetQuality(uint32_t quality) {
    settings_.quality = std::clamp<uint32_t>(quality, 1, 100);
    auto m = Info().mode;
    if (m.width) pipeline_.SetBitrate(BitrateFor(m.width, m.height));
}

void HostSession::SetOrientation(bool portrait) {
    settings_.portrait = portrait;
    ApplyOrientation(portrait);
}

void HostSession::ApplyOrientation(bool portrait) {
    SessionInfo info = Info();
    if ((info.mode.height > info.mode.width) == portrait || info.supportedModes.empty()) return;
    // Same resolution rotated if the phone supports it, otherwise the largest mode in that orientation.
    const Mode* best = nullptr;
    for (auto& m : info.supportedModes)
        if (m.width == info.mode.height && m.height == info.mode.width) best = &m;
    if (!best) {
        uint64_t area = 0;
        for (auto& m : info.supportedModes)
            if ((m.height > m.width) == portrait && uint64_t(m.width) * m.height > area) { area = uint64_t(m.width) * m.height; best = &m; }
    }
    if (best) {
        Status s = display_.SetMode(*best);
        if (!s.ok && events_.onWarning) events_.onWarning(s);
    }
}

Status HostSession::SetResolution(const Mode& m) {
    SessionInfo info = Info();
    for (auto& s : info.supportedModes)
        if (s.width == m.width && s.height == m.height) return display_.SetMode(s);
    return Status::Error(L"O celular não suporta essa resolução.");
}

}  // namespace celmon
