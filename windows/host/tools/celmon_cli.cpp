// celmon-cli — diagnostics for each pipeline stage, independent of the phone and the UI.
//   celmon-cli info                                   driver status
//   celmon-cli monitor [WxH] [Hz]                     create a virtual monitor, wait for Enter, remove it
//   celmon-cli capture [seconds] [file] [WxH] [--cursor]
//                                                     virtual monitor + test pattern -> capture -> H.264 file
//   celmon-cli verify <file.h264> [out.png]           decode with an independent decoder, save last frame
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

#include "../src/display/VirtualDisplay.h"
#include "../src/encoder/ColorConverter.h"
#include "../src/encoder/MfEncoder.h"
#include "../src/pipeline/VideoPipeline.h"
#include "../src/session/HostSession.h"
#include "../src/transport/AdbTransport.h"
#include "../src/util/Log.h"
#include "TestPattern.h"
#include "Verify.h"

using namespace celmon;

static int Fail(const Status& s) {
    fwprintf(stderr, L"ERRO: %ls\n", s.message.c_str());
    return 1;
}

static bool ParseSize(const char* s, uint32_t& w, uint32_t& h) { return sscanf_s(s, "%ux%u", &w, &h) == 2; }

static int CmdInfo() {
    VirtualDisplay vd;
    Status s = vd.Open();
    if (!s.ok) return Fail(s);
    auto info = vd.Info();
    if (!info) return Fail(Status::Error(L"sem resposta do driver"));
    printf("driver api=%u versao=%u.%u monitores ativos=%u/%u\n", info->apiVersion, info->driverVersion >> 16,
           info->driverVersion & 0xFFFF, info->activeMonitors, info->maxMonitors);
    return 0;
}

static int CmdMonitor(int argc, char** argv) {
    uint32_t w = 1920, h = 1080, hz = 60;
    if (argc > 0 && !ParseSize(argv[0], w, h)) { fprintf(stderr, "tamanho invalido: %s\n", argv[0]); return 2; }
    if (argc > 1) hz = uint32_t(atoi(argv[1]));

    std::vector<Mode> modes = {{w, h, hz}, {1920, 1080, 60}, {1600, 900, 60}, {1280, 720, 60}, {h, w, hz}};
    VirtualDisplay vd;
    Status s = vd.Add(0xC0FFEE0000000001ull, "CelMonitor", modes, 0);
    if (!s.ok) return Fail(s);

    std::wstring name;
    for (int i = 0; i < 50 && name.empty(); ++i) { Sleep(100); name = vd.GdiDeviceName(); }
    if (name.empty()) {
        printf("Monitor criado, mas o Windows ainda nao o ativou (pode estar desativado em Configuracoes > Tela).\n");
    } else {
        auto r = vd.DesktopRect();
        auto m = vd.CurrentMode();
        wprintf(L"Monitor ativo: %ls\n", name.c_str());
        if (r) printf("  area no desktop: (%ld,%ld)-(%ld,%ld)\n", r->left, r->top, r->right, r->bottom);
        if (m) printf("  modo atual: %ux%u @ %u Hz\n", m->width, m->height, m->refreshHz);
    }
    printf("Pressione Enter para remover o monitor...\n");
    std::string line;
    std::getline(std::cin, line);
    s = vd.Remove();
    if (!s.ok) return Fail(s);
    printf("Monitor removido.\n");
    return 0;
}

static std::wstring Widen(const char* s) {
    wchar_t buf[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, MAX_PATH);
    return buf;
}

static int CmdCapture(int argc, char** argv) {
    int seconds = 5;
    std::string file = "capture.h264";
    uint32_t w = 1920, h = 1080, fps = 60;
    bool moveCursor = false;
    int pos = 0;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--cursor")) { moveCursor = true; continue; }
        if (!strncmp(argv[i], "--fps=", 6)) { fps = uint32_t(atoi(argv[i] + 6)); continue; }
        if (pos == 0) seconds = atoi(argv[i]);
        else if (pos == 1) file = argv[i];
        else if (pos == 2 && !ParseSize(argv[i], w, h)) { fprintf(stderr, "tamanho invalido\n"); return 2; }
        ++pos;
    }

    VirtualDisplay vd;
    Status s = vd.Add(0xC0FFEE0000000001ull, "CelMonitor", {{w, h, 60}, {h, w, 60}}, 0);
    if (!s.ok) return Fail(s);
    std::wstring name;
    std::optional<RECT> rect;
    for (int i = 0; i < 50 && (name.empty() || !rect); ++i) { Sleep(100); name = vd.GdiDeviceName(); rect = vd.DesktopRect(); }
    if (name.empty() || !rect) return Fail(Status::Error(L"O Windows não ativou o monitor virtual."));
    wprintf(L"monitor %ls em (%ld,%ld) %ldx%ld\n", name.c_str(), rect->left, rect->top, rect->right - rect->left,
            rect->bottom - rect->top);

    TestPattern pattern;
    pattern.Start(*rect);
    Sleep(500);

    std::ofstream out(file, std::ios::binary);
    std::mutex outLock;
    uint64_t frames = 0, keyframes = 0, bytes = 0, encodeUsSum = 0, maxEncodeUs = 0;
    uint32_t streamW = 0, streamH = 0;
    std::wstring encoder;

    VideoPipeline pipe;
    PipelineConfig cfg;
    cfg.gdiName = name;
    cfg.fps = fps;
    PipelineEvents ev;
    ev.onStreamStart = [&](uint32_t sw, uint32_t sh, const std::wstring& enc) { streamW = sw; streamH = sh; encoder = enc; };
    ev.onError = [&](const Status& e) { fwprintf(stderr, L"pipeline: %ls\n", e.message.c_str()); };
    ev.onFrame = [&](EncodedFrame&& f) {
        std::lock_guard<std::mutex> lock(outLock);
        out.write(reinterpret_cast<const char*>(f.config.data()), std::streamsize(f.config.size()));
        out.write(reinterpret_cast<const char*>(f.data.data()), std::streamsize(f.data.size()));
        ++frames;
        keyframes += f.key ? 1 : 0;
        bytes += f.config.size() + f.data.size();
        encodeUsSum += f.encodeUs;
        maxEncodeUs = std::max<uint64_t>(maxEncodeUs, f.encodeUs);
    };
    uint64_t t0 = NowUs();
    unsigned long long patternStart = pattern.Frames();
    pipe.Start(cfg, ev);

    POINT saved{};
    if (moveCursor) {
        GetCursorPos(&saved);
        Sleep(seconds * 500);
        SetCursorPos((rect->left + rect->right) / 2, (rect->top + rect->bottom) / 2);
        Sleep(seconds * 500);
        SetCursorPos(saved.x, saved.y);
    } else {
        Sleep(seconds * 1000);
    }
    PipelineStats ps = pipe.Stats();
    pipe.Stop();
    double elapsed = (NowUs() - t0) / 1e6;
    printf("padrao de teste desenhou %.1f fps\n", (pattern.Frames() - patternStart) / elapsed);
    printf("diagnostico: quadros do desktop=%llu esperas[fluxo=%llu encoder=%llu fps=%llu]\n", ps.desktopFrames,
           ps.waitsFlow, ps.waitsEncoder, ps.waitsPacing);
    pattern.Stop();
    vd.Remove();

    std::lock_guard<std::mutex> lock(outLock);
    wprintf(L"encoder: %ls  stream %ux%u\n", encoder.c_str(), streamW, streamH);
    printf("frames: %llu em %.1fs (%.1f fps), keyframes: %llu\n", frames, elapsed, frames / elapsed, keyframes);
    printf("taxa: %.0f kbit/s, encode medio %.2f ms (max %.2f ms)\n", bytes * 8 / elapsed / 1000,
           frames ? encodeUsSum / 1000.0 / frames : 0.0, maxEncodeUs / 1000.0);
    printf("arquivo: %s (%llu bytes)\n", file.c_str(), bytes);
    return frames > 0 ? 0 : 1;
}

static int CmdVerify(int argc, char** argv) {
    if (argc < 1) { fprintf(stderr, "uso: verify <arquivo.h264> [saida.png]\n"); return 2; }
    std::wstring in = Widen(argv[0]);
    std::wstring png = argc > 1 ? Widen(argv[1]) : in + L".png";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto r = verify::DecodeFile(in.c_str(), png.c_str());
    printf("decodificados: %d frames, %ux%u\n", r.decodedFrames, r.width, r.height);
    if (!r.error.empty()) { printf("ERRO: %s\n", r.error.c_str()); return 1; }
    wprintf(L"ultimo frame salvo em %ls\n", png.c_str());
    return r.decodedFrames > 0 ? 0 : 1;
}

static std::atomic<bool> g_stop{false};

static std::optional<RECT> MonitorRect(const std::wstring& gdiName) {
    struct Ctx { const std::wstring* name; std::optional<RECT> rect; } ctx{&gdiName, std::nullopt};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(p);
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(m, &mi) && *c->name == mi.szDevice) { c->rect = mi.rcMonitor; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.rect;
}

// Full MVP: finds the phone over ADB, connects, creates the monitor and streams until Enter/Ctrl+C or disconnect.
static int CmdServe(int argc, char** argv) {
    SessionSettings settings;
    std::string serial;
    int seconds = 0;
    bool withPattern = false;
    TestPattern pattern;
    bool patternStarted = false;
    for (int i = 0; i < argc; ++i) {
        uint32_t w, h;
        if (!strncmp(argv[i], "--seconds=", 10)) { seconds = atoi(argv[i] + 10); continue; }
        if (!strcmp(argv[i], "--pattern")) { withPattern = true; continue; }
        if (!strcmp(argv[i], "--portrait")) settings.portrait = true;
        else if (!strncmp(argv[i], "--fps=", 6)) settings.fps = uint32_t(atoi(argv[i] + 6));
        else if (!strncmp(argv[i], "--quality=", 10)) settings.quality = uint32_t(atoi(argv[i] + 10));
        else if (!strcmp(argv[i], "--hevc")) settings.codec = proto::Codec::H265;
        else if (!strncmp(argv[i], "--serial=", 9)) serial = argv[i] + 9;
        else if (ParseSize(argv[i], w, h)) settings.mode = Mode{w, h, 60};
    }
    AdbTransport adb;
    Status s = adb.Init();
    if (!s.ok) return Fail(s);
    if (serial.empty()) {
        auto devices = adb.ListDevices();
        for (auto& d : devices) {
            if (d.state == DeviceState::Unauthorized)
                printf("Celular %s aguardando autorizacao: aceite 'Permitir depuracao USB' na tela dele.\n", d.serial.c_str());
            if (d.state == DeviceState::Ready && serial.empty()) serial = d.serial;
        }
        if (serial.empty()) return Fail(Status::Error(L"Nenhum celular pronto. Conecte o cabo e ative a Depuração USB."));
    }
    std::unique_ptr<IConnection> conn;
    s = adb.Connect(serial, conn);
    if (!s.ok) return Fail(s);

    std::atomic<bool> ended{false};
    std::wstring endReason;
    HostSession::Events ev;
    ev.onWarning = [](const Status& w) { fwprintf(stderr, L"aviso: %ls\n", w.message.c_str()); };
    ev.onEnded = [&](const std::wstring& why) { endReason = why; ended = true; };
    HostSession session(std::move(conn), settings, ev);
    session.Start();
    SetConsoleCtrlHandler([](DWORD) -> BOOL { g_stop = true; return TRUE; }, TRUE);
    printf("Transmitindo. Pressione Ctrl+C para parar.\n");
    uint64_t deadline = seconds ? NowUs() + uint64_t(seconds) * 1000000 : 0;
    while (!ended && !g_stop && (!deadline || NowUs() < deadline)) {
        Sleep(1000);
        SessionInfo i = session.Info();
        if (withPattern && !patternStarted && i.state == SessionState::Streaming) {
            if (auto r = MonitorRect(i.displayName)) { pattern.Start(*r); patternStarted = true; }
        }
        const char* st = i.state == SessionState::Streaming ? "transmitindo" : i.state == SessionState::CreatingMonitor ? "preparando" : "conectando";
        wprintf(L"[%hs] %ls %ls  %ux%u  %.1f fps  %u kbit/s  latencia %.1f ms  rtt %.1f ms  encode %.1f ms\n", st,
                i.manufacturer.c_str(), i.model.c_str(), i.mode.width, i.mode.height, i.fps, i.kbps, i.latencyUs / 1000.0,
                i.rttUs / 1000.0, i.encodeUs / 1000.0);
    }
    if (!ended) session.Stop(L"encerrado no PC");
    while (!ended) Sleep(50);
    wprintf(L"Sessao encerrada: %ls\n", endReason.c_str());
    return 0;
}

// Pipeline on an existing (physical) monitor, to separate capture/encode issues from the virtual-monitor path.
static int CmdCaptureDisplay(int argc, char** argv) {
    if (argc < 1) { fprintf(stderr, "uso: capture-display <\\\\.\\DISPLAYn> [segundos]\n"); return 2; }
    std::wstring name = Widen(argv[0]);
    int seconds = argc > 1 ? atoi(argv[1]) : 4;
    std::atomic<uint64_t> frames{0};
    VideoPipeline pipe;
    PipelineConfig cfg;
    cfg.gdiName = name;
    PipelineEvents ev;
    ev.onError = [&](const Status& e) { fwprintf(stderr, L"pipeline: %ls\n", e.message.c_str()); };
    ev.onFrame = [&](EncodedFrame&&) { ++frames; };
    uint64_t t0 = NowUs();
    pipe.Start(cfg, ev);
    Sleep(seconds * 1000);
    PipelineStats ps = pipe.Stats();
    pipe.Stop();
    double el = (NowUs() - t0) / 1e6;
    printf("%llu frames em %.1fs (%.1f fps); quadros do desktop=%llu esperas[encoder=%llu fps=%llu]\n", frames.load(), el,
           frames / el, ps.desktopFrames, ps.waitsEncoder, ps.waitsPacing);
    return 0;
}

// Encoder alone (no capture, no virtual monitor): how fast does the hardware MFT accept frames on this GPU right now?
static int CmdBenchEncoder(int argc, char** argv) {
    uint32_t w = 1920, h = 1080;
    int frames = argc > 0 ? atoi(argv[0]) : 300;
    bool convert = false, useDda = false;
    int gapMs = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--convert")) convert = true;
        if (!strcmp(argv[i], "--dda")) useDda = true;
        if (!strncmp(argv[i], "--gap=", 6)) gapMs = atoi(argv[i] + 6);
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DISPLAY_DEVICEW dd = {sizeof(dd)};
    std::wstring primary;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i)
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) primary = dd.DeviceName;
    D3DContext d3d;
    Status s = d3d.Create(primary);
    if (!s.ok) return Fail(s);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> tex[4];
    for (auto& t : tex) d3d.device->CreateTexture2D(&td, nullptr, &t);
    ComPtr<ID3D11Texture2D> bgra;
    ColorConverter conv;
    if (convert) {
        D3D11_TEXTURE2D_DESC bd = td;
        bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d3d.device->CreateTexture2D(&bd, nullptr, &bgra);
        s = conv.Init(d3d, w, h, w, h);
        if (!s.ok) return Fail(s);
    }
    DesktopDuplicator dda;
    if (useDda) {
        s = dda.Init(d3d);
        if (!s.ok) return Fail(s);
    }

    std::atomic<int> outputs{0};
    MfEncoder enc;
    EncoderConfig ec;
    ec.width = w; ec.height = h;
    s = enc.Init(d3d, ec, [&](EncodedFrame&&) { ++outputs; });
    if (!s.ok) return Fail(s);
    uint64_t t0 = NowUs(), maxWait = 0;
    for (int i = 0; i < frames; ++i) {
        if (gapMs) Sleep(gapMs);
        uint64_t w0 = NowUs();
        while (!enc.ReadyForInput() && NowUs() - w0 < 5000000) Sleep(0);
        uint64_t waited = NowUs() - w0;
        maxWait = std::max<uint64_t>(maxWait, waited);
        if (gapMs && i < 12) printf("  quadro %d: esperou %.1f ms por NeedInput\n", i, waited / 1000.0);
        if (useDda) dda.Acquire(0);
        if (convert) conv.Convert(useDda && dda.HasFrame() ? dda.Frame() : bgra.Get(), tex[i % 4].Get());
        s = enc.Encode(tex[i % 4].Get(), NowUs(), 0, i == 0);
        if (!s.ok) return Fail(s);
    }
    while (outputs < frames && NowUs() - t0 < 30000000) Sleep(1);
    double el = (NowUs() - t0) / 1e6;
    printf("%d frames em %.2fs = %.1f fps, maior espera por NeedInput %.1f ms\n", outputs.load(), el, outputs / el, maxWait / 1000.0);
    enc.Shutdown();
    return 0;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Log::Init(true);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 2) {
        printf("uso: celmon-cli info | monitor [WxH] [Hz] | capture [s] [arquivo] [WxH] [--cursor] | verify <arquivo> [png]\n");
        return 2;
    }
    if (!strcmp(argv[1], "info")) return CmdInfo();
    if (!strcmp(argv[1], "monitor")) return CmdMonitor(argc - 2, argv + 2);
    if (!strcmp(argv[1], "capture")) return CmdCapture(argc - 2, argv + 2);
    if (!strcmp(argv[1], "verify")) return CmdVerify(argc - 2, argv + 2);
    if (!strcmp(argv[1], "bench-encoder")) return CmdBenchEncoder(argc - 2, argv + 2);
    if (!strcmp(argv[1], "capture-display")) return CmdCaptureDisplay(argc - 2, argv + 2);
    if (!strcmp(argv[1], "serve")) return CmdServe(argc - 2, argv + 2);
    fprintf(stderr, "comando desconhecido: %s\n", argv[1]);
    return 2;
}
