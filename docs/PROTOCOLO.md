# Protocolo CelMonitor v1

Protocolo binário próprio, igual em qualquer transporte (ADB ou AOA/WinUSB). O transporte é tratado como **fluxo de bytes confiável e ordenado**; o protocolo faz o enquadramento.

Implementações de referência:
- C++: `common/protocol/celmon_protocol.h` (host Windows)
- Kotlin: `android/app/src/main/java/com/celmonitor/protocol/` (cliente)

Os valores numéricos nas duas implementações **devem** ser idênticos. Toda mudança passa por este documento primeiro.

## Convenções

- Inteiros em **little-endian**.
- `str` = `u16 comprimento` + bytes UTF-8 (máx. 256 bytes).
- Coordenadas de input: `u16` normalizado 0–65535 sobre a área do stream (independe da resolução).
- Tempos em microssegundos (`u64`), cada lado no seu próprio relógio monotônico.

## Cabeçalho (16 bytes)

| Offset | Tipo | Campo |
|---|---|---|
| 0 | u32 | `magic` = `0x4D4C4543` (bytes `C E L M`) |
| 4 | u16 | `type` |
| 6 | u16 | `flags` — bit 0 `IGNORABLE` |
| 8 | u32 | `length` do payload (≤ 8 MiB) |
| 12 | u32 | `seq` — contador por direção (diagnóstico) |

### Regras de validação (os dois lados)

1. `magic` errado, `length` acima do limite do tipo, ou payload menor que o mínimo do tipo → **erro de protocolo**: envia `DISCONNECT(PROTOCOL_ERROR)` e fecha a conexão. Nunca se tenta "ressincronizar".
2. Tipo desconhecido com `IGNORABLE` → descartado. Sem `IGNORABLE` → erro de protocolo.
3. Payload **maior** que o formato conhecido é aceito e os bytes extras ignorados (é assim que versões *minor* acrescentam campos no final).
4. Nenhum dado recebido é executado ou interpretado como comando fora desta tabela; valores fora de faixa (resolução, FPS, coordenadas, contagens) são rejeitados.

## Versionamento

- `HELLO` leva `major.minor`. *Major* diferente → `DISCONNECT(VERSION_MISMATCH)`.
- *Minor* só acrescenta campos no final dos payloads ou novos tipos `IGNORABLE`.

## Sequência de sessão

```
Android (cliente)                      Windows (host)
      | --------- HELLO ---------------> |  versão, aparelho, tela, modos suportados
      | <-------- HELLO_ACK ------------ |  versão aceita, id da sessão
      |                                  |  (cria o monitor virtual com os modos do HELLO)
      | <-------- STREAM_CONFIG -------- |  resolução, fps, codec, bitrate, streamId
      | --------- STREAM_READY --------> |  decoder configurado
      | <-------- VIDEO_FRAME ---------- |  (1º frame sempre IDR com SPS/PPS)
      | --------- FRAME_ACK -----------> |  controle de fluxo + latência
      |            ...                   |
      | <-------> HEARTBEAT <----------> |  a cada 1 s; 5 s sem nada = conexão perdida
      | --------- DISCONNECT ----------> |
```

Uma mudança de resolução/codec/orientação gera novo `STREAM_CONFIG` com `streamId` novo; frames com `streamId` antigo são descartados pelo cliente.

## Mensagens

### 0x0001 HELLO (cliente → host), mín. 32 bytes

| Tipo | Campo |
|---|---|
| u16 | versionMajor (1) |
| u16 | versionMinor (0) |
| u32 | capabilities — bit0 H.264, bit1 H.265, bit2 AV1, bit8 touch, bit9 decoder low-latency |
| u32 | screenWidth (px, orientação natural) |
| u32 | screenHeight |
| u32 | densityDpi |
| u32 | refreshRateMilliHz |
| u32 | rotation (0–3) |
| u16 | modeCount (1–32) |
| u16 | reservado |
| u32 | reservado |
| modeCount × 12 bytes | `u32 width, u32 height, u16 maxFps, u16 codecMask` (modos já validados pelo decoder do aparelho) |
| str | deviceId (estável por aparelho) |
| str | manufacturer |
| str | model |
| str | androidVersion |
| str | appVersion |

### 0x0002 HELLO_ACK (host → cliente), mín. 12 bytes

`u16 versionMajor, u16 versionMinor, u32 hostCapabilities, u32 sessionId, str hostName`

### 0x0003 STREAM_CONFIG (host → cliente), mín. 24 bytes

`u32 streamId, u32 width, u32 height, u16 fps, u8 codec (1=H.264, 2=H.265, 3=AV1), u8 orientation (0=paisagem, 1=retrato), u32 bitrateKbps, u32 flags`

### 0x0004 STREAM_READY (cliente → host), 8 bytes

`u32 streamId, u32 status (0 = ok; ≠0 = erro de decoder, ver códigos)`

### 0x0010 VIDEO_FRAME (host → cliente), mín. 28 bytes

`u32 streamId, u32 frameNumber, u64 captureTimeUs, u64 ptsUs, u16 flags (bit0 keyframe, bit1 codec config), u16 reservado` + dados do bitstream em Annex-B até o fim do payload.

Um frame com o bit *codec config* contém **somente** os parameter sets (SPS/PPS, e VPS no H.265) e é entregue ao decoder com `BUFFER_FLAG_CODEC_CONFIG`. O host sempre envia um codec config imediatamente antes de cada IDR.

### 0x0011 FRAME_ACK (cliente → host), 24 bytes

`u32 streamId, u32 frameNumber, u64 captureTimeUs (eco), u32 clientProcessingUs, u32 flags`

O cliente envia **dois** ACKs por frame:

- `flags = 0` quando o frame é entregue ao decoder (`clientProcessingUs` = recebido → enfileirado). É o sinal de **controle de fluxo**.
- `flags bit0 = RENDERED` quando o frame decodificado é liberado para a tela (`clientProcessingUs` = recebido → exibido). É a base da **latência** medida.

Controle de fluxo: o host mantém no máximo **N frames sem o primeiro ACK** (padrão 3). Com a janela cheia ele **não captura/codifica** novos frames — nunca descarta frames já codificados (quebraria a cadeia de referência do H.264). Não se usa o ACK de exibição para isso porque alguns decoders retêm frames antes da primeira saída, o que causaria deadlock.
Latência aproximada = `agora_host − captureTimeUs` ao receber o ACK `RENDERED`.

### 0x0012 REQUEST_KEYFRAME (cliente → host), 8 bytes

`u32 streamId, u32 reason` (1 = decoder recriado, 2 = erro de decode, 3 = surface recriada)

### 0x0020 HEARTBEAT (ambos), 24 bytes

`u64 senderTimeUs, u64 echoTimeUs, u32 counter, u32 reservado`

- `echoTimeUs = 0`: **pedido** periódico (1 s). Quem recebe responde **imediatamente** com um HEARTBEAT cujo `echoTimeUs` é o `senderTimeUs` recebido.
- `echoTimeUs ≠ 0`: **resposta**; não é respondida. RTT = `agora − echoTimeUs` (mesmo relógio de quem pediu).

Responder na hora evita que a espera do ciclo de 1 s entre na medida de RTT.

### 0x0021 CLIENT_SETTINGS (cliente → host), 12 bytes — pedido, o host decide

`u32 mask (bit0 fps, bit1 quality, bit2 orientation), u16 fps, u8 quality (1–100), u8 orientation (0 paisagem, 1 retrato), u32 reservado`

### 0x0022 HOST_STATUS (host → cliente, IGNORABLE), mín. 20 bytes

`u32 encodeFps×100, u32 bitrateKbps, u32 latencyUs, u32 flags, u32 reservado, str encoderName`

### 0x0030 INPUT_MOUSE (cliente → host), 12 bytes

`u8 action (1 move, 2 down, 3 up, 4 wheel), u8 button (1 esq., 2 dir., 3 meio), u16 reservado, u16 x, u16 y, i16 wheelV, i16 wheelH`

### 0x0031 INPUT_TOUCH (cliente → host), 4 + n×8 bytes

`u8 count (1–10), u8 reservado[3]` + `count ×` `{u8 id (0–9), u8 action (1 down, 2 move, 3 up, 4 cancel), u16 reservado, u16 x, u16 y}`

### 0x00F0 NOTICE (host → cliente, IGNORABLE)

`u16 code, u16 severity (0 info, 1 aviso, 2 erro), str mensagem` — para mostrar no celular erros do PC (ex.: "monitor virtual não criado").

### 0x00FF DISCONNECT (ambos), mín. 4 bytes

`u16 reason, u16 reservado, str mensagem`

| reason | Significado |
|---|---|
| 0 | normal (usuário) |
| 1 | PROTOCOL_ERROR |
| 2 | VERSION_MISMATCH |
| 3 | TIMEOUT |
| 4 | DISPLAY_ERROR (monitor virtual) |
| 5 | ENCODER_ERROR |
| 6 | DECODER_ERROR |
| 7 | SHUTDOWN |

## Sincronização do USB acessório (AOA)

Com ADB cada conexão é um socket TCP novo. No modo acessório USB o canal bulk é **um só e sobrevive entre sessões**: bytes de uma tentativa anterior (um HELLO antigo, o final cortado de um quadro de vídeo) podem continuar na fila. Por isso, antes do protocolo, o transporte AOA faz uma sincronização com marcadores de 16 bytes (tag ASCII de 8 bytes + `u64` nonce aleatório, little-endian):

```
PC                                            Celular
 | -- CELMSYNC(n) a cada 250 ms ---------------> |  descarta tudo até achar um CELMSYNC
 | <----------------------------- CELMSACK(n) -- |  responde UMA vez por nonce
 |  (descarta tudo até achar CELMSACK(n))        |
 | -- CELMSDON(n) -----------------------------> |  só aceita DONE do último nonce respondido
 | <------------------------------------ HELLO --|  protocolo normal a partir daqui
```

Regras do transporte AOA (limitações medidas no driver `f_accessory` do Android):

- O PC nunca envia transferências maiores que 16.000 bytes nem de tamanho múltiplo de 512; toda transferência termina num pacote curto. Não se usam pacotes de tamanho zero (kernels antigos os entregam como fim de arquivo).
- O celular lê sempre em blocos de exatamente 16 KB e **nunca chama `available()`** no descritor (`FIONREAD` não é suportado e falha com `EINVAL`).
- Como um app morto no celular deixa as escritas do PC bloqueadas para sempre, o PC detecta o timeout de 5 s num watchdog independente e aborta as transferências pendentes.

## Limites

| Item | Limite |
|---|---|
| payload | 8 MiB (VIDEO_FRAME); 4 KiB demais tipos |
| resolução | 320–7680 × 320–7680 |
| fps | 1–240 |
| modos no HELLO | 1–32 |
| contatos de touch | 1–10 |
