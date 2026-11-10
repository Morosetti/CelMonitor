# CelMonitor — guia de desenvolvimento

## Estrutura

```
common/protocol/          protocolo (C++ header-only) + vetores de teste binários
windows/
  driver/CelMonIdd/       driver do monitor virtual (UMDF 2 + IddCx)
  driver/CelMonAoa/       INF WinUSB para celulares em modo acessório (18D1:2D00/2D01)
  shared/                 interface IOCTL driver <-> app
  host/src/
    display/              VirtualDisplay: cria/remove o monitor via IOCTL, acha-o no Windows
    capture/              D3DContext (GPU do monitor), DesktopDuplicator
    encoder/              ColorConverter (BGRA->NV12 na GPU), IVideoEncoder, MfEncoder (MFT de hardware),
                          JpegEncoder (tiles JPEG das áreas que mudaram, via WIC, para aparelhos antigos)
    pipeline/             VideoPipeline: captura -> conversão -> encode, recuperação de erros
    transport/            IConnection, AdbTransport, AoaTransport (+ sincronização do canal)
    session/              HostSession: protocolo, controle de fluxo, heartbeat/watchdog
    input/                TouchInjector: toque do celular -> toque do Windows no monitor virtual
    app/                  Controller (detecção, reconexão, políticas), Settings (perfis), PhoneDetect
    ui/                   janela Win32, bandeja, configurações avançadas, atalho "janela para o celular" (WindowMover)
    util/                 log, estatísticas de processo, início automático
  host/tools/celmon_cli   diagnóstico por etapa + instalação de drivers (usado pelo instalador)
  host/tests/             testes do protocolo (gera os vetores cruzados)
  installer/              assinatura, instalação manual dos drivers, instalador Inno Setup
android/app/src/main/java/com/celmonitor/
  protocol/               espelho Kotlin do protocolo
  transport/              AdbSocketSource, AccessorySource, AccessorySync
  session/                ClientSession, ConnectionManager
  decoder/                FrameDecoder; VideoDecoder (MediaCodec -> Surface, Android 6+);
                          JpegDecoder (tiles JPEG -> cópia da tela -> Surface, Android 4.0+)
  input/                  TouchMapper (MotionEvent -> INPUT_TOUCH)
  display/                resolução/decoders do aparelho -> modos oferecidos ao PC
docs/                     análise técnica, protocolo, instalação, este guia
```

## Ferramentas

| Para | Ferramenta |
|---|---|
| Driver e app Windows | Visual Studio 2026 Build Tools: *Desktop development with C++*, Windows 11 SDK, *Spectre-mitigated libs*, componente **Windows Driver Kit** (`Component.Microsoft.Windows.DriverKit.BuildTools`). O WDK/SDK dos drivers vem do NuGet (`windows/driver/packages.config`). |
| App Android | JDK 17, Android SDK (plataforma 34, build-tools), Gradle wrapper do projeto |
| Instalador | Inno Setup 6 (`winget install JRSoftware.InnoSetup`) |

## Compilar

```powershell
.\windows\build.ps1                     # driver + app Windows (build\driver, build\host)
.\windows\build.ps1 -App                # só o app
.\windows\installer\sign-driver.ps1     # assina os pacotes de driver (certificado local, não exportável)
cd android; .\gradlew.bat assembleDebug # APK de desenvolvimento
.\windows\installer\build-installer.ps1 -Version 0.2.0   # tudo + build\installer\CelMonitor-Setup-0.2.0.exe
```

Particularidades desta máquina de desenvolvimento (documentadas nos scripts): o MSBuild de 64 bits é obrigatório para o InfVerif do WDK; o `build.ps1` remove variáveis de ambiente gigantes que quebram o MSBuild; o JDK precisa de `-Djdk.net.unixdomain.tmpdir=C:\jtmp` quando o TEMP tem nome 8.3.

## Instalar em desenvolvimento (sem o instalador)

```powershell
# como Administrador
.\windows\installer\install-driver.ps1      # confia no certificado + instala os dois drivers
adb install -r android\app\build\outputs\apk\debug\app-debug.apk
.\build\host\Release\CelMonitor.exe
```

`uninstall-driver.ps1 -RemoveCertificate` desfaz tudo.

## Depurar

| O quê | Como |
|---|---|
| Log do app Windows | `%LOCALAPPDATA%\CelMonitor\celmonitor.log` (também no DebugView) |
| Log do driver | DebugView (*Capture Global Win32*), prefixo `[CelMonIdd]` |
| Log do Android | `adb logcat -s ClientSession ConnectionManager VideoDecoder AccessorySource DeviceDisplay` |
| Encoder | `set CELMON_TRACE_ENCODER=1` (eventos do MFT) e `CELMON_TRACE_GPU=1` (tempo de GPU da conversão) |

Diagnóstico por etapa com o `celmon-cli`:

| Comando | Valida |
|---|---|
| `info` | driver instalado e respondendo |
| `monitor 1920x1080` | cria o monitor virtual (Enter remove) |
| `capture 5 teste.h264 [WxH] [--cursor] [--fps=N]` | monitor + padrão animado + captura + encode num arquivo |
| `verify teste.h264 saida.png` | decodifica com decoder independente e salva o último quadro |
| `bench-encoder 300 [--convert] [--dda] [--gap=ms]` | desempenho do encoder isolado |
| `capture-display \\.\DISPLAY1 4` | pipeline num monitor físico |
| `aoa-probe [--switch]` | interfaces ADB/acessório; `--switch` ativa o modo acessório |
| `pattern \\.\DISPLAYn 10` | padrão animado num monitor existente (medir FPS/latência) |
| `serve [--pattern] [--portrait] [--seconds=N]` | sessão completa por ADB, sem a interface |
| `setup-drivers <pasta>` / `remove-drivers [--remove-cert]` | usados pelo instalador (Administrador) |

## Testes

- `build\host\Release\celmon-tests.exe` — protocolo C++; gera `common/protocol/testvectors/`.
- `android\gradlew testDebugUnitTest` — protocolo Kotlin (inclui os vetores do C++: bytes idênticos nos dois lados) e sincronização do acessório.
- Roteiro manual (celular real): Windows 10/11; Android 6+ (H.264) e um aparelho Android 4.x (JPEG); USB 2.0 e 3.x; paisagem/retrato; troca de resolução com a transmissão ativa; desconectar/reconectar o cabo; tela do celular apagando; fechar o app do celular; jogo usando a GPU; resolução não suportada; driver ausente; ADB ausente.

## Adicionar um codec

1. `common/protocol/celmon_protocol.h` e `Protocol.kt`: valor em `Codec` e bit em `Capability` (documente em `PROTOCOLO.md`).
2. Windows: implemente `IVideoEncoder` (ex.: `NvencEncoder`) ou estenda `MfEncoder::Init` com o subtipo MF; escolha o encoder em `VideoPipeline::SetupEncoder`.
3. Android: `Codec` já mapeia o MIME; `DeviceDisplay.buildModes` passa a marcar o bit nos modos que o decoder aceita.
4. UI: opção no combo *Codec* (`MainWindow`) e no perfil (`Settings`).

## Toque (entrada)

O toque já funciona: `TouchMapper` (Android) gera `INPUT_TOUCH` a partir dos `MotionEvent` da `SurfaceView`; `HostSession::OnTouch` valida o estado e chama `TouchInjector`, que injeta toque do Windows no retângulo do monitor virtual (`VirtualDisplay::DesktopRect`). Para outro tipo de entrada (ex.: mouse/touchpad com `INPUT_MOUSE`):

1. Protocolo: mensagem em `celmon_protocol.h`/`Protocol.kt` com validação de faixa no parser e teste.
2. Android: gerar o evento e enviar pela `ConnectionManager`.
3. Windows: tratar em `HostSession::Handle`, só com o stream ativo, sempre limitado ao monitor virtual.

## Logo e ícones

`windows\host\res\make-icon.ps1` desenha a logo por código (monitor + celular com uma janela atravessando as duas telas) e gera: `CelMonitor.ico` (colorido: exe, barra de tarefas, bandeja transmitindo), `CelMonitorIdle.ico` (cinza: bandeja/janela sem transmissão), `docs\logo.png` e os vetores do Android (`ic_launcher_foreground.xml`, `logo.xml`). Os arquivos gerados são versionados; rode o script só para mudar a logo (`-Preview arquivo.png` gera uma prancha com todos os tamanhos em fundo claro e escuro).

## Publicar uma versão

1. Atualize a versão em `windows/host/res/CelMonitor.rc` e `android/app/build.gradle.kts` (`versionName` e **sempre aumente o `versionCode`**: é por ele que o CelMonitor decide atualizar o app nos celulares; o instalador grava esse número em `CelMonitor.apk.version`).
2. `.\windows\installer\build-installer.ps1 -Version X.Y.Z`
3. Teste o instalador numa máquina limpa (ou VM) seguindo `INSTALACAO.md`.
