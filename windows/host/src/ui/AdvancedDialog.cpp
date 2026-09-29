#include "AdvancedDialog.h"

#include <algorithm>
#include <string>

namespace celmon {

namespace {

enum : int { CmbMode = 1, ChkLowLatency, EdtTransfer, EdtBitrate, ChkDefault, BtnReset, BtnSave, BtnCancel };

struct DialogState {
    Controller* controller;
    DeviceProfile profile;
    HFONT font;
    UINT dpi;
    bool done = false;
};

int Scale(const DialogState& s, int v) { return MulDiv(v, s.dpi, 96); }

void Load(HWND h, const DeviceProfile& p) {
    SendDlgItemMessageW(h, CmbMode, CB_SETCURSEL, p.transport == TransportMode::Auto ? 0 : p.transport == TransportMode::AoaOnly ? 1 : 2, 0);
    CheckDlgButton(h, ChkLowLatency, p.session.decoderLowLatency ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(h, EdtTransfer, p.usbMaxTransfer, FALSE);
    SetDlgItemInt(h, EdtBitrate, p.session.maxBitrateKbps / 1000, FALSE);
}

bool Read(HWND h, DeviceProfile& p) {
    int mode = int(SendDlgItemMessageW(h, CmbMode, CB_GETCURSEL, 0, 0));
    p.transport = mode == 1 ? TransportMode::AoaOnly : mode == 2 ? TransportMode::AdbOnly : TransportMode::Auto;
    p.session.decoderLowLatency = IsDlgButtonChecked(h, ChkLowLatency) == BST_CHECKED;
    BOOL ok1 = FALSE, ok2 = FALSE;
    UINT transfer = GetDlgItemInt(h, EdtTransfer, &ok1, FALSE);
    UINT mbps = GetDlgItemInt(h, EdtBitrate, &ok2, FALSE);
    if (!ok1 || transfer < 512 || transfer > 65536) {
        MessageBoxW(h, L"O tamanho da transferência USB deve estar entre 512 e 65536 bytes.", L"CelMonitor", MB_ICONWARNING);
        return false;
    }
    if (!ok2 || mbps > 100) {
        MessageBoxW(h, L"A taxa de bits máxima deve estar entre 0 (automática) e 100 Mbit/s.", L"CelMonitor", MB_ICONWARNING);
        return false;
    }
    p.usbMaxTransfer = transfer;
    p.session.maxBitrateKbps = mbps * 1000;
    return true;
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto* s = reinterpret_cast<DialogState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_COMMAND:
        if (!s) break;
        switch (LOWORD(w)) {
        case BtnReset:
            Load(h, DeviceProfile{});
            return 0;
        case BtnSave: {
            DeviceProfile p = s->profile;
            if (!Read(h, p)) return 0;
            s->controller->SetProfile(p, IsDlgButtonChecked(h, ChkDefault) == BST_CHECKED);
            DestroyWindow(h);
            return 0;
        }
        case BtnCancel:
        case IDCANCEL:
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (s) s->done = true;
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

}  // namespace

void ShowAdvancedDialog(HWND owner, Controller& controller, HFONT font, UINT dpi) {
    // The owner keeps receiving posted commands while the dialog runs its modal loop: never open a second one.
    if (HWND open = FindWindowW(L"CelMonitorAdvanced", nullptr)) {
        SetForegroundWindow(open);
        return;
    }
    DialogState st{&controller, controller.Profile(), font, dpi};
    std::string serial = controller.ProfileSerial();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"CelMonitorAdvanced";
    RegisterClassW(&wc);

    const int m = Scale(st, 14), w = Scale(st, 480), labelW = Scale(st, 250), rowH = Scale(st, 26);
    RECT rc = {0, 0, w, Scale(st, 400)};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&rc, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
    RECT ownerRc;
    GetWindowRect(owner, &ownerRc);
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Configurações avançadas deste celular", style,
                             ownerRc.left + Scale(st, 30), ownerRc.top + Scale(st, 60), rc.right - rc.left, rc.bottom - rc.top,
                             owner, nullptr, wc.hInstance, nullptr);
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));

    int y = m;
    auto add = [&](int id, const wchar_t* cls, const wchar_t* text, DWORD s, int x, int yy, int cw, int ch) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | s, x, yy, cw, ch, h, reinterpret_cast<HMENU>(INT_PTR(id)),
                                 wc.hInstance, nullptr);
        SendMessageW(c, WM_SETFONT, WPARAM(font), TRUE);
        return c;
    };
    std::wstring who = serial.empty() ? L"Padrão para novos celulares" : L"Celular: " + std::wstring(serial.begin(), serial.end());
    add(0, L"STATIC", who.c_str(), SS_LEFT, m, y, w - 2 * m, rowH);
    y += rowH + Scale(st, 6);

    add(0, L"STATIC", L"Modo de conexão USB:", SS_LEFT, m, y + Scale(st, 4), labelW, rowH);
    HWND cmb = add(CmbMode, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, m, y + rowH, w - 2 * m, Scale(st, 150));
    for (const wchar_t* t : {L"Automático (USB direto quando possível, senão ADB)", L"Somente USB direto (acessório/AOA)",
                             L"Somente ADB (mais compatível)"})
        SendMessageW(cmb, CB_ADDSTRING, 0, LPARAM(t));
    y += 2 * rowH + Scale(st, 10);

    add(ChkLowLatency, L"BUTTON", L"Usar otimizações de baixa latência do decoder do celular", BS_AUTOCHECKBOX | WS_TABSTOP, m, y,
        w - 2 * m, rowH);
    y += rowH;
    add(0, L"STATIC", L"Desmarque se a imagem travar ou ficar corrompida neste celular.", SS_LEFT, m + Scale(st, 18), y, w - 2 * m, rowH);
    y += rowH + Scale(st, 8);

    add(0, L"STATIC", L"Tamanho máximo da transferência USB (bytes):", SS_LEFT, m, y + Scale(st, 4), labelW + Scale(st, 60), rowH);
    add(EdtTransfer, L"EDIT", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, w - m - Scale(st, 110), y, Scale(st, 110), rowH);
    y += rowH;
    add(0, L"STATIC", L"Padrão 16000. Reduza se o USB direto cair ao enviar vídeo.", SS_LEFT, m + Scale(st, 18), y, w - 2 * m, rowH);
    y += rowH + Scale(st, 8);

    add(0, L"STATIC", L"Taxa de bits máxima (Mbit/s, 0 = automática):", SS_LEFT, m, y + Scale(st, 4), labelW + Scale(st, 60), rowH);
    add(EdtBitrate, L"EDIT", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, w - m - Scale(st, 110), y, Scale(st, 110), rowH);
    y += rowH + Scale(st, 12);

    add(ChkDefault, L"BUTTON", L"Usar também como padrão para novos celulares", BS_AUTOCHECKBOX | WS_TABSTOP, m, y, w - 2 * m, rowH);
    y += rowH + Scale(st, 4);
    add(0, L"STATIC", L"As mudanças valem a partir da próxima conexão.", SS_LEFT, m, y, w - 2 * m, rowH);
    y += rowH + Scale(st, 10);

    const int bw = Scale(st, 130), bh = Scale(st, 30);
    add(BtnReset, L"BUTTON", L"Restaurar padrões", BS_PUSHBUTTON | WS_TABSTOP, m, y, bw, bh);
    add(BtnCancel, L"BUTTON", L"Cancelar", BS_PUSHBUTTON | WS_TABSTOP, w - m - bw, y, bw, bh);
    add(BtnSave, L"BUTTON", L"Salvar", BS_DEFPUSHBUTTON | WS_TABSTOP, w - m - 2 * bw - Scale(st, 8), y, bw, bh);
    y += bh + m;

    rc = {0, 0, w, y};
    AdjustWindowRectExForDpi(&rc, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
    SetWindowPos(h, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
    Load(h, st.profile);

    // Modal loop.
    EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    MSG msg;
    while (!st.done) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
            PostQuitMessage(int(msg.wParam));  // keep the app's quit request for the main loop
            break;
        }
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (IsWindow(h)) DestroyWindow(h);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
}

}  // namespace celmon
