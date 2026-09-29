# CelMonitor — instalação e primeiros passos

## Requisitos

- Windows 10 versão 2004 ou mais recente, 64 bits (Windows 11 incluso).
- Placa de vídeo com encoder H.264 por hardware (praticamente todas as NVIDIA, AMD e Intel dos últimos 10 anos).
- Celular com Android 10 ou mais recente.
- Cabo USB **de dados** (alguns cabos só carregam).
- Internet durante a instalação (para baixar o ADB do Google).

## 1. Instalar no PC

1. Execute `CelMonitor-Setup-<versão>.exe` e aceite o pedido de administrador.
2. Leia a página *Informações* (explica o que será instalado) e avance.
3. Deixe marcado **"Iniciar o CelMonitor junto com o Windows"** se quiser que ele fique sempre pronto na bandeja.
4. Ao final, deixe marcado **"Abrir o CelMonitor agora"**.

O instalador faz tudo sozinho: programa, drivers (monitor virtual e USB), confiança no certificado dos drivers, download do ADB e o app para o celular.

> O Windows SmartScreen pode avisar que o instalador "não é reconhecido", porque ele não é assinado por um certificado comercial. Clique em **Mais informações > Executar assim mesmo** se você confia na origem do arquivo.

## 2. Preparar o celular (uma única vez)

Ative a **Depuração USB**. A janela do CelMonitor mostra, em **"Próximo passo"**, o caminho exato para a marca do seu celular, reconhecida automaticamente quando você conecta o cabo. Em resumo:

| Marca | Onde fica |
|---|---|
| Xiaomi / Redmi / POCO | Sobre o telefone > toque 7× em *Versão do MIUI* → Configurações adicionais > Opções do desenvolvedor > *Depuração USB* (e *Instalar via USB*) |
| Samsung | Sobre o telefone > Informações do software > toque 7× em *Número de compilação* → Opções do desenvolvedor > *Depuração USB* |
| Motorola, Google, Nokia e outros | Sobre o telefone > toque 7× em *Número da versão* → Sistema > Opções do desenvolvedor > *Depuração USB* |

## 3. Conectar

1. Conecte o cabo. Na primeira vez, o celular pergunta **"Permitir depuração USB?"**: marque *Sempre permitir deste computador* e toque em **Permitir**.
2. O CelMonitor instala o app no celular (se precisar), abre o app e ativa a conexão USB direta.
3. Se o celular perguntar **"Abrir CelMonitor para este acessório USB?"**, marque *Usar por padrão* e toque em **OK**.
4. Pronto: o monitor aparece no Windows e a imagem aparece no celular.

Posicione o monitor novo em relação às outras telas em *Configurações > Sistema > Tela* (arraste o retângulo dele). A escala do texto também se ajusta lá.

Da próxima vez basta plugar o cabo: se o CelMonitor estiver na bandeja, ele conecta sozinho.

## Uso no dia a dia

- **Bandeja do Windows:** o ícone fica **azul enquanto transmite** e **cinza** quando não há celular transmitindo; passe o mouse para ver o estado. Clique para abrir; botão direito para *Conectar/Desconectar*, *Iniciar com o Windows* e *Sair*.
- **Fechar a janela (X)** só a esconde; o CelMonitor continua na bandeja. Para encerrar, use *Sair*.
- **No celular**, o botão ▼ no canto abre o menu: sair do modo monitor, orientação, qualidade, FPS e informações da conexão.
- **Na janela do PC:** o topo mostra o estado, o celular e os números ao vivo (resolução, FPS, atraso); abaixo, *Próximo passo* diz o que fazer e *Tela do celular* tem resolução, orientação, FPS máximo, qualidade e codec. Tudo fica salvo **por celular**. Os números técnicos (taxa, encode, RTT, CPU/GPU, encoder) ficam em **Detalhes técnicos**.

## Celular diferente? Configurações avançadas

Cada celular tem o seu próprio perfil. Se algo não funcionar bem num aparelho, ajuste em **"Configurações avançadas..."** na janela (valem para o celular conectado):

| Opção | Quando mudar |
|---|---|
| **Modo de conexão USB** — Automático / Somente USB direto / Somente ADB | *Somente ADB* se o celular não aceitar o modo acessório ou a conexão direta cair; *Somente USB direto* para nunca usar ADB. |
| **Otimizações de baixa latência do decoder** | Desmarque se a imagem travar, piscar ou ficar corrompida (alguns decoders de fabricantes não se dão bem com elas). |
| **Tamanho máximo da transferência USB** | Padrão 16000. Reduza (ex.: 8000 ou 4000) se a conexão direta cair logo que o vídeo começa. |
| **Taxa de bits máxima** | Limite (ex.: 10 Mbit/s) se usar hub USB, cabo longo ou se o celular esquentar. |

*"Usar também como padrão para novos celulares"* aplica os mesmos valores a aparelhos conectados pela primeira vez. As mudanças valem a partir da próxima conexão. As configurações ficam em `%LOCALAPPDATA%\CelMonitor\settings.ini` (uma seção por celular).

## Problemas comuns

| Sintoma | O que fazer |
|---|---|
| "Conecte o celular…" mesmo com o cabo plugado | Troque o cabo/porta (cabo de dados). Veja se a Depuração USB está ativa. |
| "Autorize a depuração USB no celular" | Desbloqueie o celular e toque em *Permitir*. Se não aparecer, reconecte o cabo. |
| "Driver do monitor virtual não instalado" | Execute o instalador de novo. |
| "ADB não encontrado" | Execute o instalador de novo com internet. |
| Imagem congela ou fica corrompida | Configurações avançadas: desmarque *otimizações do decoder*; ou use *Somente ADB*. |
| Conexão direta cai ao começar o vídeo | Configurações avançadas: reduza o *tamanho da transferência USB*. |
| Imagem escura/amarelada | É o *Luz Noturna* do Windows ou o *Modo de leitura* do celular. |
| Transferência de arquivos (MTP) sumiu | Enquanto a conexão direta está ativa o celular fica em modo acessório; desconecte e reconecte o cabo para voltar ao normal. |

Log detalhado: botão **Pasta de logs** (arquivo `celmonitor.log`).

## Desinstalar

*Configurações do Windows > Aplicativos > CelMonitor > Desinstalar.* Remove o programa, os drivers, o certificado e o início automático. O app do celular pode ser desinstalado normalmente no Android.

## Limitações conhecidas

- **Depuração USB é necessária.** É por ela que o PC pede ao celular para entrar em modo acessório; sem ela, o Windows não dá acesso ao celular para um programa comum (ele fica preso ao driver de arquivos/MTP). Ver `ANALISE_TECNICA.md` §4.
- **Tela segura do Windows** (UAC, Ctrl+Alt+Del, tela de bloqueio) não é transmitida.
- **Conteúdo protegido** (alguns players de streaming com DRM) aparece preto no monitor do celular.
- **Jogo pesado na mesma GPU** pode reduzir o FPS do monitor do celular: o próprio Windows passa a compor esse monitor com menos frequência.
- **Distribuição:** os drivers usam um certificado próprio. Para distribuir amplamente sem a etapa de confiança no certificado, é preciso certificado EV e assinatura da Microsoft (fora do escopo do código).
