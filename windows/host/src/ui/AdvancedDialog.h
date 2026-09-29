// "Configurações avançadas deste celular": per-phone options for phones that need different settings to work well.
#pragma once

#include <windows.h>

#include "../app/Controller.h"

namespace celmon {

// Modal; returns after the user saves or cancels. Changes apply from the next connection.
void ShowAdvancedDialog(HWND owner, Controller& controller, HFONT font, UINT dpi);

}  // namespace celmon
