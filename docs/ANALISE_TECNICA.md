# CelMonitor — Análise técnica

Objetivo: fazer um celular Android, ligado por cabo USB, funcionar como **monitor adicional real** do Windows 10/11 (o Windows enxerga um display a mais em *Configurações > Tela*), com baixa latência e, numa segunda fase, touch como entrada.

---

## 1. Como criar o monitor virtual no Windows

### Opções avaliadas

| Opção | O Windows vê um monitor? | Situação | Veredito |
|---|---|---|---|
| **Indirect Display Driver (IddCx, UMDF v2)** | **Sim**, monitor de verdade, com modos, EDID, arranjo e persistência | Modelo oficial da Microsoft desde o Win10 1607, feito justamente para "USB displays" e "remote displays" | **Escolhido** |
| Mirror driver (XDDM) | Não (só espelha) | Removido no Windows 8+ | Inviável |
| Driver WDDM completo (kernel) | Sim | Enorme, kernel-mode, exige assinatura WHQL | Desproporcional |
| Plugue HDMI "dummy" + streaming | Sim (hardware) | Funciona, mas depende de hardware extra | Só como contorno |
| Apenas transmitir a tela (DDA/WGC sem driver) | **Não** — só espelha um monitor existente | É o erro que o enunciado pede para evitar | Rejeitado |
| Driver IDD de terceiros já assinado (ex.: Virtual-Display-Driver, Parsec VDD, usbmmidd) | Sim | Funciona hoje, mas sem controle de modos por celular, sem ligação com a sessão USB, licenças variadas | Aceitável só como plano B de desenvolvimento |

### Como funciona o IDD

- É uma DLL **UMDF v2** (modo usuário, roda em `WUDFHost.exe` na Session 0). Um crash do driver **não** derruba o sistema.
- Usa a extensão de classe **IddCx**: o driver cria um *adapter*, anuncia monitores (`IddCxMonitorCreate` + `IddCxMonitorArrival`), responde quais modos cada monitor suporta e remove monitores (`IddCxMonitorDeparture`).
- O Windows compõe o desktop daquele monitor e entrega ao driver uma **swap chain Direct3D** (`IddCxSwapChainReleaseAndAcquireBuffer`) — ou seja, o próprio driver recebe os frames na GPU.

### Desenho do nosso driver (`CelMonIdd`)

- Dispositivo de software enumerado pela raiz (`Root\CelMonIdd`), instalado uma única vez.
- Expõe uma *device interface* própria com IOCTLs validados:
  - `GET_INFO` (versão do driver/protocolo),
  - `ADD_MONITOR` (lista de modos derivada do celular, modo preferido, id estável do celular),
  - `REMOVE_MONITOR`.
- **Monitores pertencem ao handle que os criou**: se o app do Windows fechar ou travar, o driver recebe `EvtFileCleanup` e remove o monitor. Nunca fica "monitor fantasma".
- Gera um **EDID** por celular (nome "CelMonitor", serial derivado do id do aparelho) para o Windows lembrar posição/resolução de cada celular.
- Modos: resolução nativa do celular (retrato e paisagem), 1920×1080, 1600×900, 1280×720 etc., em 30/60 Hz — sempre respeitando a proporção do aparelho para evitar distorção.
- Orientação retrato/paisagem é oferecida como **modos nativos** (ex.: 1080×2400 e 2400×1080), não via rotação do Windows, que complicaria a captura.
- Alvo **IddCx 1.4** (Windows 10 2004+ e Windows 11).

### Assinatura do driver (limitação importante)

- Drivers UMDF não passam pela política de assinatura de kernel. Para **desenvolvimento/uso pessoal**, basta um catálogo assinado com um certificado próprio instalado em *Trusted Root* + *Trusted Publishers* da máquina — **não é preciso ativar test mode nem desligar Secure Boot** (é o mesmo método usado pelos forks do IddSampleDriver).
- Para **distribuir a terceiros** de forma limpa, é necessário certificado EV + *attestation signing* no Microsoft Partner Center (custo anual). Isso não é resolvível por código.

---

## 2. Como capturar os frames desse monitor

| Opção | Latência | Observações |
|---|---|---|
| **Desktop Duplication API (DXGI, `IDXGIOutput1::DuplicateOutput`)** | Muito baixa | Entrega textura D3D11 na GPU + *dirty rects* + forma/posição do cursor separadas. Funciona em monitores IDD. **Escolhido para o MVP.** |
| Windows Graphics Capture | Baixa | Borda amarela no Win10, um pouco mais de overhead; bom plano B. |
| Frames direto da swap chain do IDD | Mínima | Driver copia para textura compartilhada (NT handle + keyed mutex) lida pelo app. Funciona inclusive na tela segura (UAC). Mais complexo → **evolução pós-MVP**, atrás da mesma interface `IFrameSource`. |

Detalhes do pipeline de captura (tudo na GPU, sem cópia para CPU):

1. `AcquireNextFrame` → `CopyResource` para um anel de 3 texturas próprias (triple buffering; libera o frame do DWM imediatamente).
2. Cursor: o DDA não inclui o ponteiro na imagem; desenhamos o cursor por shader na textura copiada.
3. Conversão **BGRA → NV12** e escala pelo `ID3D11VideoProcessor` (hardware de vídeo da GPU).
4. A textura NV12 vai direto ao encoder.
5. Tratamento de `DXGI_ERROR_ACCESS_LOST` (troca de modo, tela segura, troca de resolução) recriando a duplicação.

O adaptador de render do monitor virtual será a mesma GPU que tem o encoder (`IddCxAdapterSetRenderAdapter`), para evitar cópias entre GPUs em notebooks híbridos.

---

## 3. Encoding

| Codec | Uso |
|---|---|
| **H.264** (padrão) | Suportado por 100% dos celulares Android 10+, decodificação em hardware. |
| H.265/HEVC | Opcional: ~30–40% menos banda; nem todo aparelho tem decoder HEVC. |
| AV1 | Adiado: pouquíssimos encoders (RTX 40+/Arc) e decoders móveis. |
| MJPEG | Não recomendado: consome CPU dos dois lados e mais banda; só faria sentido como diagnóstico. |

Implementação: **Media Foundation, MFT de hardware assíncrono** (NVIDIA/AMD/Intel pelo mesmo código) com `IMFDXGIDeviceManager` (entrada D3D11 zero-cópia), `CODECAPI_AVLowLatencyMode`, sem B-frames, GOP longo com IDR sob demanda, taxa de bits configurável. Fallback: MFT de software da Microsoft (CPU) — com aviso na UI.
A interface `IVideoEncoder` permite depois acrescentar NVENC/AMF/QSV nativos sem mexer no resto.

Banda: 1080p60 em H.264 ≈ 8–20 Mbit/s (1–2,5 MB/s). USB 2.0 entrega ~35 MB/s na prática → **USB 2.0 é suficiente**; USB 3 só ajuda em resoluções muito altas.

---

## 4. Como transportar por USB

### O ponto delicado (limitação real do Windows)

No USB o PC é sempre o *host* e o celular o *device*. O app do Windows precisa conversar com o celular, mas **o Windows associa o celular ao driver MTP**, e aplicações comuns não conseguem enviar comandos a um dispositivo preso a esse driver.

| Opção | Precisa de "Depuração USB"? | Prós | Contras |
|---|---|---|---|
| **ADB** (`adb forward` para um socket do app) | Sim | Funciona em qualquer aparelho; robusto; permite até abrir o app automaticamente | Exige Opções do desenvolvedor; depende do adb.exe |
| **Android Open Accessory (AOA 2.0) + WinUSB** | Só para o "empurrão" inicial (ver abaixo) | Canal bulk USB dedicado, sem servidor adb; o Android abre o app sozinho ao conectar; é o mecanismo feito para isso | Para mandar o comando "entre em modo acessório", o Windows precisa de um handle WinUSB do celular |
| Tethering USB (rede RNDIS/NCM) | Não, mas o usuário liga manualmente | IP sobre cabo | Desvia a internet do PC para o celular, alguns planos bloqueiam, o app não consegue ligar sozinho |
| libusb puro | — | — | Mesmo problema do driver MTP; libusb só abre dispositivos WinUSB/libusbK/UsbDk |

Como destravar o AOA no Windows:

1. **Via interface ADB** (se a Depuração USB estiver ligada): a interface ADB já usa WinUSB; mandamos por ela os *control transfers* AOA (51/52/53). O celular reaparece como `18D1:2D00/2D01` (VID/PID fixos do Google para *qualquer* marca).
2. **Via UsbDk** (filtro USB assinado, Apache-2.0, usado pelo QEMU/virt-viewer): permite mandar os comandos AOA **sem Depuração USB**. Custo: instalar um filtro que se anexa a todos os dispositivos USB.
3. Depois da troca, o `18D1:2D00/2D01` é associado ao **WinUSB por um INF nosso** (assinado com o mesmo certificado do driver) — isso é universal, independe da marca do celular.

### Recomendação

- Camada `ITransport` com o mesmo protocolo por cima.
- **Primário (produto): AOA 2.0 + WinUSB bulk**, com "empurrão" via interface ADB ou UsbDk.
- **Fallback: ADB forward**, que também é a forma mais rápida de validar o MVP e testar com o emulador.
- Conclusão honesta: **com o Windows "de fábrica", não existe forma totalmente sem configuração** de falar com um app Android por USB. Ou o usuário liga a Depuração USB uma vez, ou o instalador instala o UsbDk. (Produtos comerciais como Duet e Splashtop também pedem Depuração USB no Android com cabo.)

---

## 5. Decodificação e renderização no Android

- **Kotlin**, sem NDK (não há ganho: o decode é em hardware e o render é direto na Surface).
- `MediaCodec` assíncrono → saída direto na `Surface` de um **`SurfaceView`** (caminho zero-cópia, composição por overlay de hardware; `TextureView` adiciona uma cópia GPU e latência).
- Baixa latência: `KEY_LOW_LATENCY` (Android 11+), chaves de fabricante (ex.: Qualcomm `vendor.qti-ext-dec-low-latency.enable`), `KEY_PRIORITY=0`, render imediato sem fila.
- Verificar `VideoCapabilities.isSizeSupported()` antes de oferecer um modo (ex.: 1080×2400 exige H.264 nível 5.1).
- Imagem ajustada com *letterbox* mantendo proporção — nunca esticada.
- Tela cheia imersiva, `FLAG_KEEP_SCREEN_ON`, reconstrução do decoder quando a Surface é destruída (tela apagou/app em segundo plano) e pedido de IDR ao PC.

---

## 6. Touch (Fase 2)

| Mecanismo | Avaliação |
|---|---|
| **`SendInput` (mouse absoluto no desktop virtual)** | Robusto, sem driver, fácil de mapear para as coordenadas do monitor virtual. **Modo padrão "mouse".** |
| **`InjectSyntheticPointerInput` (touch sintético, Win10 1809+)** | Multitouch real; o Windows faz gestos nativos (pinça, rolagem, segurar = botão direito). **Modo opcional "touch".** |
| HID virtual (VHF) | Exige driver **kernel** → assinatura Microsoft obrigatória. Nenhum ganho sobre as duas acima. Rejeitado. |
| AOA HID | Serve para o PC controlar o *celular*, é o sentido oposto. Não se aplica. |

Os gestos (toque = clique, arrastar = mover, segurar = botão direito, dois dedos = rolagem) são reconhecidos no Android; o PC recebe eventos já normalizados (coordenadas 0–65535 relativas ao monitor), valida e injeta.
Limitação: sem rodar como administrador, o Windows (UIPI) bloqueia injeção em janelas elevadas; a tela segura (UAC/Ctrl+Alt+Del) nunca recebe input sintético.

---

## 7. Componentes nativos e drivers

| Componente | Linguagem | Nativo? |
|---|---|---|
| Driver IDD `CelMonIdd` | C++ (UMDF2 + IddCx) | Sim — obrigatório |
| INF WinUSB para `18D1:2D00/2D01` | INF (sem binário próprio) | Usa `winusb.sys` da Microsoft |
| App host (captura, encode, USB, input, UI) | C++20 + Win32 | Sim — D3D11/DXGI/MF/WinUSB são APIs COM/C |
| App Android | Kotlin | Não precisa de NDK |
| Protocolo | Especificação + implementação C++ e Kotlin espelhadas | — |

Drivers a instalar no PC: `CelMonIdd` (nosso), INF WinUSB do modo acessório (nosso), opcionalmente UsbDk. Nada precisa ser instalado no Android além do APK.

## 8. Maiores dificuldades técnicas

1. **Driver IDD**: ciclo de vida de monitor, modos, EDID, IOCTLs, instalação e assinatura.
2. **Bootstrap do USB no Windows** (MTP x WinUSB), detalhado na seção 4.
3. **Latência ponta a ponta**: controle de fluxo (não enfileirar frames; *back-pressure* na captura), IDR sob demanda, variação entre decoders de fabricantes.
4. Recuperação de erros: troca de modo, tela segura, cabo removido, tela do celular apagada, driver ausente.
5. Variedade de hardware Android (limites de resolução do decoder, suporte a HEVC, AOA em alguns fabricantes).
6. Tela segura/UAC não capturável pelo DDA em processo comum (evolução: frames pela swap chain do driver).

## 9. Stack recomendada

- **Windows:** C++20, MSBuild/Visual Studio 2022+, WDK (NuGet) para o driver; Win32 para a UI (um único executável, sem runtime extra). C# seria agradável para a UI, mas obrigaria uma ponte C++/C# para o pipeline de vídeo inteiro; Rust ainda não tem suporte maduro a IddCx.
- **Android:** Kotlin, minSdk 29 (Android 10), MediaCodec + SurfaceView, API de USB Accessory.
- **Vídeo:** H.264 por MFT de hardware; HEVC opcional.
- **USB:** AOA 2.0 + WinUSB bulk; ADB forward como fallback.

## 10. Ordem de implementação (cada passo validado antes do próximo)

1. Toolchains (VS C++ + Windows SDK + WDK, JDK 17).
2. `common/protocol`: especificação + C++ + Kotlin + testes de ida e volta.
3. Driver IDD: adicionar/remover monitor por IOCTL → **validar**: aparece em Configurações > Tela, troca de resolução funciona.
4. Host: captura DDA do monitor virtual → encode H.264 → arquivo → **validar** o vídeo gerado.
5. Android: decoder + SurfaceView (testável no emulador) → **validar** reprodução.
6. Transporte ADB ponta a ponta → **MVP funcionando**.
7. Transporte AOA + INF WinUSB → **validar** com celular real, sem adb.
8. UI completa, estatísticas, configurações, tratamento de erros, reconexão.
9. Fase 2: touch (mouse → touch sintético).
10. Instalador (scripts) e documentação final.
