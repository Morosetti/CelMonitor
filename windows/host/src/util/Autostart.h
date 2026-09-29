// "Start with Windows": HKCU\Software\Microsoft\Windows\CurrentVersion\Run\CelMonitor = "<exe>" --minimized
#pragma once

namespace celmon::Autostart {

bool IsEnabled();
bool SetEnabled(bool enabled);

}  // namespace celmon::Autostart
