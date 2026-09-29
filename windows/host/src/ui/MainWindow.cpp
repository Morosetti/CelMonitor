#include "MainWindow.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>

#include "../util/Log.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace celmon {

namespace {

enum : int {
    kTimer = 1,
    // group boxes
    GrpConn = 100, GrpMonitor, GrpPerf,
    // connection
    LblStatus = 200, ValStatus, LblPhone, ValPhone, LblLink, ValLink, LblMonitor, ValMonitor, BtnConnect, LblTransport, CmbTransport,
    // monitor settings
    LblRes = 300, CmbRes, LblOrient, CmbOrient, LblFps, CmbFps, LblQuality, CmbQuality, LblCodec, CmbCodec, NoteCodec,
    // performance
    LblCurFps = 400, ValCurFps, LblLatency, ValLatency, LblRate, ValRate, LblEncode, ValEncode, LblRtt, ValRtt, LblCpu,
    ValCpu, LblEncoder, ValEncoder,
    // footer
    ValMessage = 500, BtnLogs,
};

constexpr int kQualities[] = {25, 50, 80, 100};
constexpr int kFps[] = {30, 60};

std::wstring Fmt(const wchar_t* fmt, double v) {
    wchar_t b[64];
    swprintf_s(b, fmt, v);
    return b;
}

std::wstring ModeText(const Mode& m, uint32_t phoneW, uint32_t phoneH) {
    std::wstring s = std::to_wstring(m.width) + L" × " + std::to_wstring(m.height);
    s += m.height > m.width ? L"  (retrato" : L"  (paisagem";
    bool native = (m.width == phoneW && m.height == phoneH) || (m.width == phoneH && m.height == phoneW);
    s += native ? L", nativa)" : L")";
    return s;
}

}  // namespace

bool MainWindow::Create(HINSTANCE instance, int show) {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    bg_ = GetSysColorBrush(COLOR_WINDOW);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = bg_;
    wc.lpszClassName = L"CelMonitorMain";
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"CelMonitor", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    dpi_ = GetDpiForWindow(hwnd_);
    RecreateFonts();
    CreateControls();
    Layout();
    ShowWindow(hwnd_, show);
    SetTimer(hwnd_, kTimer, 500, nullptr);
    Refresh();
    return true;
}

LRESULT CALLBACK MainWindow::WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(msg, w, l) : DefWindowProcW(h, msg, w, l);
}

void MainWindow::RecreateFonts() {
    if (font_) DeleteObject(font_);
    if (bold_) DeleteObject(bold_);
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    bold_ = CreateFontIndirectW(&ncm.lfMessageFont);
}

void MainWindow::CreateControls() {
    auto make = [&](int id, const wchar_t* cls, const wchar_t* text, DWORD style) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                                 reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
        ctl_[id] = c;
    };
    make(GrpConn, L"BUTTON", L"Conexão", BS_GROUPBOX);
    make(GrpMonitor, L"BUTTON", L"Monitor no celular", BS_GROUPBOX);
    make(GrpPerf, L"BUTTON", L"Desempenho", BS_GROUPBOX);
    const std::pair<int, const wchar_t*> labels[] = {
        {LblStatus, L"Status:"}, {LblPhone, L"Celular:"}, {LblLink, L"Conexão USB:"}, {LblMonitor, L"Monitor virtual:"}, {LblTransport, L"Modo USB:"},
        {LblRes, L"Resolução:"}, {LblOrient, L"Orientação:"}, {LblFps, L"FPS máximo:"}, {LblQuality, L"Qualidade:"},
        {LblCodec, L"Codec:"}, {LblCurFps, L"FPS atual:"}, {LblLatency, L"Latência aprox.:"}, {LblRate, L"Taxa:"},
        {LblEncode, L"Encode:"}, {LblRtt, L"RTT USB:"}, {LblCpu, L"CPU / GPU:"}, {LblEncoder, L"Encoder:"},
    };
    for (auto& [id, text] : labels) make(id, L"STATIC", text, SS_LEFT);
    for (int id : {ValStatus, ValPhone, ValLink, ValMonitor, ValCurFps, ValLatency, ValRate, ValEncode, ValRtt, ValCpu, ValEncoder})
        make(id, L"STATIC", L"—", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX);
    make(NoteCodec, L"STATIC", L"(próxima conexão)", SS_LEFT);
    for (int id : {CmbRes, CmbOrient, CmbFps, CmbQuality, CmbCodec, CmbTransport}) make(id, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
    make(BtnConnect, L"BUTTON", L"Conectar", BS_PUSHBUTTON | WS_TABSTOP);
    make(ValMessage, L"STATIC", L"", SS_LEFT | SS_NOPREFIX);
    make(BtnLogs, L"BUTTON", L"Abrir pasta de logs", BS_PUSHBUTTON | WS_TABSTOP);

    updating_ = true;
    FillCombo(CmbOrient, {L"Paisagem", L"Retrato"});
    FillCombo(CmbFps, {L"30", L"60"});
    FillCombo(CmbQuality, {L"Baixa (menos banda)", L"Média", L"Alta", L"Máxima"});
    FillCombo(CmbCodec, {L"H.264 (compatível)", L"H.265 / HEVC (menos banda)"});
    AppSettings s = controller_.Settings();
    SelectCombo(CmbOrient, s.session.portrait ? 1 : 0);
    SelectCombo(CmbFps, s.session.fps <= 30 ? 0 : 1);
    int q = 0;
    for (int i = 0; i < 4; ++i)
        if (std::abs(int(s.session.quality) - kQualities[i]) < std::abs(int(s.session.quality) - kQualities[q])) q = i;
    SelectCombo(CmbQuality, q);
    SelectCombo(CmbCodec, s.session.codec == proto::Codec::H265 ? 1 : 0);
    FillCombo(CmbTransport, {L"Automático (USB direto quando possível)", L"Somente ADB"});
    SelectCombo(CmbTransport, s.transport == TransportMode::AdbOnly ? 1 : 0);
    comboModes_.clear();
    if (s.session.mode) comboModes_.push_back(*s.session.mode);
    std::vector<std::wstring> res = {L"Automática (nativa do celular)"};
    for (auto& m : comboModes_) res.push_back(ModeText(m, 0, 0));
    FillCombo(CmbRes, res);
    SelectCombo(CmbRes, comboModes_.empty() ? 0 : 1);
    updating_ = false;
}

void MainWindow::Layout() {
    for (auto& [id, h] : ctl_) SendMessageW(h, WM_SETFONT, WPARAM(font_), TRUE);
    SendMessageW(ctl_[ValStatus], WM_SETFONT, WPARAM(bold_), TRUE);

    const int m = Scale(12), labelW = Scale(118), valueW = Scale(330), rowH = Scale(26), comboH = Scale(200);
    const int innerX = m + Scale(12), valueX = innerX + labelW, width = innerX + labelW + valueW + Scale(12);
    int y = m;
    auto place = [&](int id, int x, int yy, int w, int h) { MoveWindow(ctl_[id], x, yy, w, h, FALSE); };
    auto row = [&](int label, int value, int w = 0) {
        place(label, innerX, y + Scale(3), labelW, rowH - Scale(4));
        place(value, valueX, y, w ? w : valueW, rowH);
        y += rowH;
    };

    int top = y;
    y += Scale(22);
    row(LblStatus, ValStatus);
    row(LblPhone, ValPhone);
    row(LblLink, ValLink);
    row(LblMonitor, ValMonitor);
    place(LblTransport, innerX, y + Scale(4), labelW, rowH - Scale(4));
    place(CmbTransport, valueX, y, valueW, comboH);
    y += rowH + Scale(6);
    place(BtnConnect, valueX, y + Scale(4), Scale(140), Scale(30));
    y += Scale(44);
    place(GrpConn, m, top, width - m, y - top);

    y += Scale(8);
    top = y;
    y += Scale(22);
    for (auto [l, c] : {std::pair{LblRes, CmbRes}, {LblOrient, CmbOrient}, {LblFps, CmbFps}, {LblQuality, CmbQuality}}) {
        place(l, innerX, y + Scale(4), labelW, rowH - Scale(4));
        place(c, valueX, y, c == CmbRes ? valueW : Scale(200), comboH);
        y += rowH + Scale(6);
    }
    place(LblCodec, innerX, y + Scale(4), labelW, rowH - Scale(4));
    place(CmbCodec, valueX, y, Scale(200), comboH);
    place(NoteCodec, valueX + Scale(208), y + Scale(4), valueW - Scale(208), rowH - Scale(4));
    y += rowH + Scale(14);
    place(GrpMonitor, m, top, width - m, y - top);

    y += Scale(8);
    top = y;
    y += Scale(22);
    const int half = (labelW + valueW) / 2;
    auto pair = [&](int l1, int v1, int l2, int v2) {
        place(l1, innerX, y + Scale(3), Scale(100), rowH - Scale(4));
        place(v1, innerX + Scale(100), y, half - Scale(100), rowH);
        place(l2, innerX + half, y + Scale(3), Scale(100), rowH - Scale(4));
        place(v2, innerX + half + Scale(100), y, half - Scale(100), rowH);
        y += rowH;
    };
    pair(LblCurFps, ValCurFps, LblLatency, ValLatency);
    pair(LblRate, ValRate, LblEncode, ValEncode);
    pair(LblRtt, ValRtt, LblCpu, ValCpu);
    place(LblEncoder, innerX, y + Scale(3), Scale(100), rowH - Scale(4));
    place(ValEncoder, innerX + Scale(100), y, labelW + valueW - Scale(100), rowH);
    y += rowH + Scale(10);
    place(GrpPerf, m, top, width - m, y - top);

    y += Scale(10);
    place(ValMessage, m, y, width - m, Scale(52));
    y += Scale(56);
    place(BtnLogs, m, y, Scale(160), Scale(28));
    y += Scale(28) + m;

    RECT rc = {0, 0, width + m, y};
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::SetText(int id, const std::wstring& text) {
    auto& cached = textCache_[id];
    if (cached == text) return;  // avoids flicker on the 500 ms refresh
    cached = text;
    SetWindowTextW(ctl_[id], text.c_str());
}

void MainWindow::FillCombo(int id, const std::vector<std::wstring>& items) {
    HWND c = ctl_[id];
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (auto& s : items) SendMessageW(c, CB_ADDSTRING, 0, LPARAM(s.c_str()));
}

void MainWindow::SelectCombo(int id, int index) {
    if (SendMessageW(ctl_[id], CB_GETCURSEL, 0, 0) != index) SendMessageW(ctl_[id], CB_SETCURSEL, WPARAM(index), 0);
}

void MainWindow::Refresh() {
    ControllerState st = controller_.State();
    const SessionInfo& si = st.session;
    const bool streaming = st.hasSession && si.state == SessionState::Streaming;

    std::wstring status = st.status;
    if (st.hasSession) {
        status = si.state == SessionState::Streaming ? L"Transmitindo" : si.state == SessionState::Handshaking ? L"Conectando..."
                                                                                                                 : L"Preparando o monitor virtual...";
    }
    SetText(ValStatus, status);

    std::wstring phone = L"—";
    if (st.hasSession && !si.model.empty())
        phone = si.manufacturer + L" " + si.model + L" · Android " + si.androidVersion + L" · tela " +
                std::to_wstring(si.phoneWidth) + L"×" + std::to_wstring(si.phoneHeight);
    else if (!st.deviceModel.empty())
        phone = st.deviceModel + L" (" + std::wstring(st.serial.begin(), st.serial.end()) + L")";
    SetText(ValPhone, phone);
    SetText(ValLink, st.hasSession ? si.transport : (st.serial.empty() ? L"nenhum celular detectado" : L"celular detectado — aguardando"));
    std::wstring mon = L"Inativo";
    if (st.hasSession && !si.displayName.empty()) {
        mon = L"Ativo — " + si.displayName;
        if (si.mode.width) mon += L" (" + std::to_wstring(si.mode.width) + L"×" + std::to_wstring(si.mode.height) + L")";
    }
    SetText(ValMonitor, mon);
    SetText(BtnConnect, st.hasSession ? L"Desconectar" : L"Conectar");

    // Resolution list follows what the connected phone can decode.
    HWND res = ctl_[CmbRes];
    if (st.hasSession && !si.supportedModes.empty() && !SendMessageW(res, CB_GETDROPPEDSTATE, 0, 0)) {
        updating_ = true;
        if (comboModes_.size() != si.supportedModes.size() ||
            !std::equal(comboModes_.begin(), comboModes_.end(), si.supportedModes.begin(),
                        [](const Mode& a, const Mode& b) { return a.width == b.width && a.height == b.height; })) {
            comboModes_ = si.supportedModes;
            std::vector<std::wstring> items = {L"Automática (nativa do celular)"};
            for (auto& m : comboModes_) items.push_back(ModeText(m, si.phoneWidth, si.phoneHeight));
            FillCombo(CmbRes, items);
        }
        AppSettings s = controller_.Settings();
        int sel = 0;
        if (s.session.mode)
            for (size_t i = 0; i < comboModes_.size(); ++i)
                if (comboModes_[i].width == s.session.mode->width && comboModes_[i].height == s.session.mode->height) sel = int(i) + 1;
        SelectCombo(CmbRes, sel);
        if (si.mode.width && !SendMessageW(ctl_[CmbOrient], CB_GETDROPPEDSTATE, 0, 0))
            SelectCombo(CmbOrient, si.mode.height > si.mode.width ? 1 : 0);
        updating_ = false;
    }

    // Statistics
    if (GetTickCount64() - lastStatsSample_ >= 1000) {
        stats_.Sample();
        lastStatsSample_ = GetTickCount64();
    }
    if (streaming) {
        SetText(ValCurFps, si.fps < 1 ? L"0 (tela parada)" : Fmt(L"%.0f", si.fps));
        SetText(ValLatency, si.latencyUs ? Fmt(L"%.0f ms", si.latencyUs / 1000.0) : L"—");
        SetText(ValRate, si.kbps >= 1000 ? Fmt(L"%.1f Mbit/s", si.kbps / 1000.0) : Fmt(L"%.0f kbit/s", si.kbps));
        SetText(ValEncode, si.encodeUs ? Fmt(L"%.1f ms", si.encodeUs / 1000.0) : L"—");
        SetText(ValRtt, si.rttUs ? Fmt(L"%.1f ms", si.rttUs / 1000.0) : L"—");
        SetText(ValEncoder, si.encoder.empty() ? L"—" : si.encoder);
    } else {
        for (int id : {ValCurFps, ValLatency, ValRate, ValEncode, ValRtt}) SetText(id, L"—");
        SetText(ValEncoder, st.hasSession ? si.encoder : L"—");
    }
    auto gpu = stats_.GpuPercent();
    SetText(ValCpu, Fmt(L"%.1f%%", stats_.CpuPercent()) + L" / " + (gpu ? Fmt(L"%.0f%%", *gpu) : L"n/d"));

    if (messageIsError_ != st.messageIsError) {
        messageIsError_ = st.messageIsError;
        InvalidateRect(ctl_[ValMessage], nullptr, TRUE);
    }
    SetText(ValMessage, st.message);
}

void MainWindow::OnCommand(int id, int code) {
    if (id == BtnConnect && code == BN_CLICKED) {
        if (controller_.State().hasSession) controller_.Disconnect();
        else controller_.Connect();
        return;
    }
    if (id == BtnLogs && code == BN_CLICKED) {
        std::wstring path = Log::FilePath();
        std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
        ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (code != CBN_SELCHANGE || updating_) return;
    int sel = int(SendMessageW(ctl_[id], CB_GETCURSEL, 0, 0));
    if (sel < 0) return;
    switch (id) {
    case CmbRes:
        controller_.SetResolution(sel == 0 || size_t(sel) > comboModes_.size() ? std::nullopt : std::optional<Mode>(comboModes_[sel - 1]));
        break;
    case CmbOrient: controller_.SetOrientation(sel == 1); break;
    case CmbFps: controller_.SetFps(uint32_t(kFps[sel])); break;
    case CmbQuality: controller_.SetQuality(uint32_t(kQualities[sel])); break;
    case CmbCodec: controller_.SetCodec(sel == 1 ? proto::Codec::H265 : proto::Codec::H264); break;
    case CmbTransport: controller_.SetTransport(sel == 1 ? TransportMode::AdbOnly : TransportMode::Auto); break;
    }
}

LRESULT MainWindow::Handle(UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_TIMER:
        Refresh();
        return 0;
    case WM_COMMAND:
        OnCommand(LOWORD(w), HIWORD(w));
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(w);
        SetBkMode(dc, TRANSPARENT);
        if (reinterpret_cast<HWND>(l) == ctl_[ValMessage] && messageIsError_) SetTextColor(dc, RGB(196, 43, 28));
        return reinterpret_cast<LRESULT>(bg_);
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(w);
        RecreateFonts();
        auto* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_CLOSE:
        KillTimer(hwnd_, kTimer);
        SetText(ValStatus, L"Encerrando...");
        UpdateWindow(hwnd_);
        controller_.Shutdown();  // ends the session gracefully and removes the virtual monitor
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, w, l);
}

}  // namespace celmon
