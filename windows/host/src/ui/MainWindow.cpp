#include "MainWindow.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>

#include "../util/Log.h"
#include "../util/Autostart.h"
#include "AdvancedDialog.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace celmon {

namespace {

enum : int {
    kTimer = 1,
    // group boxes
    GrpMonitor = 100, GrpPerf,
    // header: logo (painted), status, phone summary, live numbers, main button
    ValStatus = 200, ValPhone, ValLive, BtnConnect,
    // monitor settings
    LblRes = 300, CmbRes, LblOrient, CmbOrient, LblFps, CmbFps, LblQuality, CmbQuality, LblCodec, CmbCodec, NoteCodec,
    // technical details (collapsible)
    LblCurFps = 400, ValCurFps, LblLatency, ValLatency, LblRate, ValRate, LblEncode, ValEncode, LblRtt, ValRtt, LblCpu,
    ValCpu, LblEncoder, ValEncoder, LblLink, ValLink, LblMonitor, ValMonitor,
    // messages, guide, footer
    ValMessage = 500, BtnLogs, BtnAdvanced, GrpHelp, ValHelp, BtnDetails,
};

// Controls of the "Detalhes técnicos" box, shown only when expanded.
constexpr int kDetailIds[] = {GrpPerf, LblCurFps, ValCurFps, LblLatency, ValLatency, LblRate, ValRate, LblEncode, ValEncode,
                              LblRtt, ValRtt, LblCpu, ValCpu, LblEncoder, ValEncoder, LblLink, ValLink, LblMonitor, ValMonitor};

constexpr int kQualities[] = {25, 50, 80, 100};
constexpr int kFps[] = {30, 60};
constexpr int kLogoSize = 64;  // header logo, in 96-dpi pixels

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

bool MainWindow::Create(HINSTANCE instance, int show, bool startHidden) {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    bg_ = GetSysColorBrush(COLOR_WINDOW);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hbrBackground = bg_;
    wc.lpszClassName = L"CelMonitorMain";
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"CelMonitor", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    dpi_ = GetDpiForWindow(hwnd_);
    showDetails_ = GetAppFlag(L"showDetails", false);
    RecreateFonts();
    LoadIcons();
    CreateControls();
    Layout();
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();
    if (startHidden) trayNotified_ = true;  // started with Windows: stay quietly in the tray
    else ShowWindow(hwnd_, show);
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
    if (big_) DeleteObject(big_);
    ncm.lfMessageFont.lfHeight = ncm.lfMessageFont.lfHeight * 3 / 2;
    big_ = CreateFontIndirectW(&ncm.lfMessageFont);
}

void MainWindow::LoadIcons() {
    for (HICON* i : {&logoLive_, &logoIdle_, &smallLive_, &smallIdle_})
        if (*i) DestroyIcon(*i), *i = nullptr;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    LoadIconWithScaleDown(inst, MAKEINTRESOURCEW(1), Scale(kLogoSize), Scale(kLogoSize), &logoLive_);
    LoadIconWithScaleDown(inst, MAKEINTRESOURCEW(2), Scale(kLogoSize), Scale(kLogoSize), &logoIdle_);
    LoadIconMetric(inst, MAKEINTRESOURCEW(1), LIM_SMALL, &smallLive_);
    LoadIconMetric(inst, MAKEINTRESOURCEW(2), LIM_SMALL, &smallIdle_);
}

// The logo tells the state at a glance: in the header, the title bar/taskbar button and the tray.
void MainWindow::SetStreamingLook(bool streaming) {
    if (streaming == streamingLook_) return;
    streamingLook_ = streaming;
    HICON icon = streaming ? smallLive_ : smallIdle_;
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, LPARAM(icon));
    NOTIFYICONDATAW nid = TrayData();
    nid.uFlags = NIF_ICON;
    nid.hIcon = icon;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    InvalidateRect(hwnd_, &logoRect_, TRUE);
}

void MainWindow::Paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    if (HICON logo = streamingLook_ ? logoLive_ : logoIdle_)
        DrawIconEx(dc, logoRect_.left, logoRect_.top, logo, logoRect_.right - logoRect_.left, logoRect_.bottom - logoRect_.top, 0,
                   nullptr, DI_NORMAL);
    EndPaint(hwnd_, &ps);
}

void MainWindow::CreateControls() {
    auto make = [&](int id, const wchar_t* cls, const wchar_t* text, DWORD style) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                                 reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
        ctl_[id] = c;
    };
    make(GrpMonitor, L"BUTTON", L"Tela do celular", BS_GROUPBOX);
    make(GrpPerf, L"BUTTON", L"Detalhes técnicos", BS_GROUPBOX);
    const std::pair<int, const wchar_t*> labels[] = {
        {LblLink, L"Conexão:"}, {LblMonitor, L"Monitor virtual:"},
        {LblRes, L"Resolução:"}, {LblOrient, L"Orientação:"}, {LblFps, L"FPS máximo:"}, {LblQuality, L"Qualidade:"},
        {LblCodec, L"Codec:"}, {LblCurFps, L"FPS atual:"}, {LblLatency, L"Latência:"}, {LblRate, L"Taxa:"},
        {LblEncode, L"Encode:"}, {LblRtt, L"RTT USB:"}, {LblCpu, L"CPU / GPU:"}, {LblEncoder, L"Encoder:"},
    };
    for (auto& [id, text] : labels) make(id, L"STATIC", text, SS_LEFT);
    for (int id : {ValStatus, ValPhone, ValLive, ValLink, ValMonitor, ValCurFps, ValLatency, ValRate, ValEncode, ValRtt, ValCpu, ValEncoder})
        make(id, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX);
    make(NoteCodec, L"STATIC", L"Mudanças de codec valem a partir da próxima conexão.", SS_LEFT);
    for (int id : {CmbRes, CmbOrient, CmbFps, CmbQuality, CmbCodec}) make(id, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
    make(BtnConnect, L"BUTTON", L"Conectar", BS_PUSHBUTTON | WS_TABSTOP);
    make(ValMessage, L"STATIC", L"", SS_LEFT | SS_NOPREFIX);
    make(BtnDetails, L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP);
    make(BtnAdvanced, L"BUTTON", L"Configurações avançadas...", BS_PUSHBUTTON | WS_TABSTOP);
    make(BtnLogs, L"BUTTON", L"Pasta de logs", BS_PUSHBUTTON | WS_TABSTOP);
    make(GrpHelp, L"BUTTON", L"Próximo passo", BS_GROUPBOX);
    make(ValHelp, L"STATIC", L"", SS_LEFT | SS_NOPREFIX);

    updating_ = true;
    FillCombo(CmbOrient, {L"Paisagem", L"Retrato"});
    FillCombo(CmbFps, {L"30", L"60"});
    FillCombo(CmbQuality, {L"Baixa (menos banda)", L"Média", L"Alta", L"Máxima"});
    FillCombo(CmbCodec, {L"H.264 (compatível)", L"H.265 (menos banda)"});
    DeviceProfile s = controller_.Profile();
    SelectCombo(CmbOrient, s.session.portrait ? 1 : 0);
    SelectCombo(CmbFps, s.session.fps <= 30 ? 0 : 1);
    int q = 0;
    for (int i = 0; i < 4; ++i)
        if (std::abs(int(s.session.quality) - kQualities[i]) < std::abs(int(s.session.quality) - kQualities[q])) q = i;
    SelectCombo(CmbQuality, q);
    SelectCombo(CmbCodec, s.session.codec == proto::Codec::H265 ? 1 : 0);
    comboModes_.clear();
    if (s.session.mode) comboModes_.push_back(*s.session.mode);
    std::vector<std::wstring> res = {L"Automática (nativa do celular)"};
    for (auto& m : comboModes_) res.push_back(ModeText(m, 0, 0));
    FillCombo(CmbRes, res);
    SelectCombo(CmbRes, comboModes_.empty() ? 0 : 1);
    updating_ = false;
}

// One column, top to bottom in order of importance: state (logo, status, phone, live numbers, main button),
// what to do next, the phone screen settings, and the technical details only when the user asks for them.
void MainWindow::Layout() {
    for (auto& [id, h] : ctl_) SendMessageW(h, WM_SETFONT, WPARAM(font_), TRUE);
    SendMessageW(ctl_[ValStatus], WM_SETFONT, WPARAM(big_), TRUE);
    SendMessageW(ctl_[ValPhone], WM_SETFONT, WPARAM(bold_), TRUE);

    const int m = Scale(16), W = Scale(560), pad = Scale(12), rowH = Scale(26), comboH = Scale(200);
    auto place = [&](int id, int x, int yy, int w, int h) { MoveWindow(ctl_[id], x, yy, w, h, FALSE); };
    int y = m;

    // ---- header
    const int logo = Scale(kLogoSize), btnW = Scale(130);
    logoRect_ = {m, y, m + logo, y + logo};
    const int tx = m + logo + Scale(16), tw = m + W - btnW - Scale(12) - tx;
    place(ValStatus, tx, y, tw, Scale(30));
    place(ValPhone, tx, y + Scale(32), tw, Scale(20));
    place(ValLive, tx, y + Scale(52), tw, Scale(20));
    place(BtnConnect, m + W - btnW, y + (logo - Scale(34)) / 2, btnW, Scale(34));
    y += logo + Scale(10);
    place(ValMessage, m, y, W, Scale(34));
    y += Scale(38);

    // ---- next step
    int top = y;
    place(ValHelp, m + pad, y + Scale(22), W - 2 * pad, Scale(92));
    y += Scale(22) + Scale(92) + Scale(8);
    place(GrpHelp, m, top, W, y - top);
    y += Scale(10);

    // ---- phone screen settings: two columns of label + combo
    top = y;
    y += Scale(24);
    const int labelW = Scale(92), colB = m + W / 2 + Scale(8);
    const int comboA = colB - Scale(16) - (m + pad + labelW), comboB = m + W - pad - (colB + labelW);
    auto field = [&](int label, int combo, int x, int w) {
        place(label, x, y + Scale(4), labelW, rowH - Scale(4));
        place(combo, x + labelW, y, w, comboH);
    };
    field(LblRes, CmbRes, m + pad, W - 2 * pad - labelW);
    y += rowH + Scale(8);
    field(LblOrient, CmbOrient, m + pad, comboA);
    field(LblFps, CmbFps, colB, comboB);
    y += rowH + Scale(8);
    field(LblQuality, CmbQuality, m + pad, comboA);
    field(LblCodec, CmbCodec, colB, comboB);
    y += rowH + Scale(6);
    place(NoteCodec, colB, y, W / 2 - Scale(8) - pad, Scale(34));
    y += Scale(34) + Scale(6);
    place(GrpMonitor, m, top, W, y - top);
    y += Scale(10);

    // ---- footer buttons
    SetText(BtnDetails, showDetails_ ? L"Ocultar detalhes técnicos  ▴" : L"Detalhes técnicos  ▾");
    place(BtnDetails, m, y, Scale(190), Scale(28));
    place(BtnAdvanced, m + Scale(198), y, Scale(200), Scale(28));
    place(BtnLogs, m + W - Scale(120), y, Scale(120), Scale(28));
    y += Scale(28);

    // ---- technical details (collapsible)
    for (int id : kDetailIds) ShowWindow(ctl_[id], showDetails_ ? SW_SHOWNA : SW_HIDE);
    if (showDetails_) {
        y += Scale(10);
        top = y;
        y += Scale(22);
        const int half = (W - 2 * pad) / 2, lw = Scale(96), x = m + pad;
        auto pair = [&](int l1, int v1, int l2, int v2) {
            place(l1, x, y + Scale(3), lw, rowH - Scale(4));
            place(v1, x + lw, y, half - lw, rowH);
            place(l2, x + half, y + Scale(3), lw, rowH - Scale(4));
            place(v2, x + half + lw, y, half - lw, rowH);
            y += rowH;
        };
        auto full = [&](int l, int v) {
            place(l, x, y + Scale(3), lw, rowH - Scale(4));
            place(v, x + lw, y, W - 2 * pad - lw, rowH);
            y += rowH;
        };
        pair(LblCurFps, ValCurFps, LblLatency, ValLatency);
        pair(LblRate, ValRate, LblEncode, ValEncode);
        pair(LblRtt, ValRtt, LblCpu, ValCpu);
        full(LblEncoder, ValEncoder);
        full(LblLink, ValLink);
        full(LblMonitor, ValMonitor);
        y += Scale(8);
        place(GrpPerf, m, top, W, y - top);
    }

    y += m;
    RECT rc = {0, 0, W + 2 * m, y};
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
    UpdateTrayTip(status);
    SetStreamingLook(streaming);

    // Header: which phone, how it is connected, and (while streaming) what the phone is showing.
    std::wstring phone = L"Nenhum celular conectado";
    if (st.hasSession && !si.model.empty())
        phone = si.manufacturer + L" " + si.model + L"  ·  Android " + si.androidVersion + L"  ·  " + si.transport;
    else if (!st.deviceModel.empty())
        phone = st.deviceModel;
    else if (!st.serial.empty())
        phone = L"Celular detectado";
    SetText(ValPhone, phone);
    std::wstring live;
    if (streaming && si.mode.width) {
        live = std::to_wstring(si.mode.width) + L" × " + std::to_wstring(si.mode.height) + L"  ·  " +
               (si.fps < 1 ? std::wstring(L"tela parada") : Fmt(L"%.0f fps", si.fps));
        if (si.latencyUs) live += Fmt(L"  ·  ~%.0f ms de atraso", si.latencyUs / 1000.0);
    }
    SetText(ValLive, live);
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
        DeviceProfile s = controller_.Profile();
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
    std::wstring help = st.help;
    if (help.empty() && st.hasSession)
        help = L"Tudo pronto. Arraste janelas para o monitor do celular; a posição dele em relação às outras telas "
               L"pode ser ajustada em Configurações do Windows > Sistema > Tela.";
    SetText(ValHelp, help);
}

void MainWindow::OnCommand(int id, int code) {
    if (id == BtnConnect && code == BN_CLICKED) {
        if (controller_.State().hasSession) controller_.Disconnect();
        else controller_.Connect();
        return;
    }
    if (id == BtnDetails && code == BN_CLICKED) {
        showDetails_ = !showDetails_;
        SetAppFlag(L"showDetails", showDetails_);
        Layout();
        return;
    }
    if (id == BtnAdvanced && code == BN_CLICKED) {
        ShowAdvancedDialog(hwnd_, controller_, font_, dpi_);
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
    case WM_PAINT:
        Paint();
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(w);
        HWND c = reinterpret_cast<HWND>(l);
        SetBkMode(dc, TRANSPARENT);
        if (c == ctl_[ValMessage] && messageIsError_) SetTextColor(dc, RGB(196, 43, 28));
        else if (c == ctl_[ValLive] || c == ctl_[NoteCodec]) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        return reinterpret_cast<LRESULT>(bg_);
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(w);
        RecreateFonts();
        LoadIcons();
        bool look = streamingLook_;
        streamingLook_ = !look;  // force SetStreamingLook to re-apply the reloaded icons
        SetStreamingLook(look);
        auto* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_CLOSE:
        // The window's X only hides it: CelMonitor keeps running in the tray and connects whenever the phone is
        // plugged in. Leaving is "Sair" in the tray menu (or CelMonitor.exe --exit).
        ShowWindow(hwnd_, SW_HIDE);
        if (!trayNotified_) {
            trayNotified_ = true;
            NOTIFYICONDATAW nid = TrayData();
            nid.uFlags = NIF_INFO;
            wcscpy_s(nid.szInfoTitle, L"CelMonitor continua ativo");
            wcscpy_s(nid.szInfo, L"Ele fica aqui na bandeja e conecta sozinho quando o celular é plugado. Para encerrar, use \"Sair\".");
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
        return 0;
    case kMsgExit:
        ExitApp();
        return 0;
    case WM_ENDSESSION:
        if (w) ExitApp();  // Windows shutting down or the installer closing us (Restart Manager)
        return 0;
    case kMsgTray:
        if (LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == NIN_SELECT || LOWORD(l) == NIN_KEYSELECT) ShowFromTray();
        else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) ShowTrayMenu();
        return 0;
    case WM_DESTROY: {
        NOTIFYICONDATAW nid = TrayData();
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    }
    if (msg == taskbarCreated_ && taskbarCreated_) {  // Explorer restarted: the tray icon must be added again
        AddTrayIcon();
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, w, l);
}

NOTIFYICONDATAW MainWindow::TrayData() const {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = 1;
    return nid;
}

void MainWindow::AddTrayIcon() {
    NOTIFYICONDATAW nid = TrayData();
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kMsgTray;
    nid.hIcon = streamingLook_ ? smallLive_ : smallIdle_;
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"CelMonitor");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void MainWindow::UpdateTrayTip(const std::wstring& status) {
    std::wstring tip = L"CelMonitor — " + status;
    if (tip == trayTip_) return;
    trayTip_ = tip;
    NOTIFYICONDATAW nid = TrayData();
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void MainWindow::ShowFromTray() {
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
}

void MainWindow::ShowTrayMenu() {
    enum : UINT { MOpen = 1, MConnect, MAutostart, MExit };
    bool connected = controller_.State().hasSession;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MOpen, L"Abrir o CelMonitor");
    AppendMenuW(menu, MF_STRING, MConnect, connected ? L"Desconectar" : L"Conectar");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (Autostart::IsEnabled() ? MF_CHECKED : 0), MAutostart, L"Iniciar com o Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MExit, L"Sair");
    SetMenuDefaultItem(menu, MOpen, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);  // required for the menu to close when clicking elsewhere
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
    case MOpen: ShowFromTray(); break;
    case MConnect: connected ? controller_.Disconnect() : controller_.Connect(); break;
    case MAutostart: Autostart::SetEnabled(!Autostart::IsEnabled()); break;
    case MExit: ExitApp(); break;
    }
}

void MainWindow::ExitApp() {
    if (exiting_) return;
    exiting_ = true;
    KillTimer(hwnd_, kTimer);
    SetText(ValStatus, L"Encerrando...");
    UpdateWindow(hwnd_);
    controller_.Shutdown();  // ends the session gracefully and removes the virtual monitor
    DestroyWindow(hwnd_);
}

}  // namespace celmon
