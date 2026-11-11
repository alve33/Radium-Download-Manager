<p align="center">
  <img src="assets/branding/RDM_Logo.svg" alt="Logo Oficial do Radium Download Manager" width="128" height="128">
</p>

<p align="center">
  <picture>
    <img alt="Radium Download Manager" src="https://img.shields.io/badge/RADIUM-DOWNLOAD%20MANAGER-e03131?style=for-the-badge&logo=windows11&logoColor=white" height="42">
  </picture>
</p>

<h1 align="center">Radium Download Manager</h1>

<p align="center">
  <a href="README.md">English</a> · <a href="README.ja.md">日本語</a> · <a href="README.zh-CN.md">简体中文</a> · <a href="README.zh-TW.md">繁體中文</a> · <a href="README.ko.md">한국어</a> · <a href="README.es.md">Español</a> · <a href="README.fr.md">Français</a> · <b>Português</b> · <a href="README.de-DE.md">Deutsch</a> · <a href="README.ru-RU.md">Русский</a> · <a href="README.it-IT.md">Italiano</a> · <a href="README.ar-SA.md">العربية</a>
</p>

<p align="center">
  <b>Gerenciador de downloads rápido e simples para Windows 11 com captura integrada de vídeos.</b><br>
  Acelere downloads com múltiplas conexões e capture vídeos em navegadores suportados (HLS, DASH e transmissões web).<br>
  Aplicativo nativo em C++20 e Win32, sem a sobrecarga do Electron, com consumo mínimo de RAM e totalmente silencioso.
</p>

<p align="center">
  <a href="https://github.com/sayedalve/Radium-Download-Manager/releases/latest"><img alt="Último lançamento" src="https://img.shields.io/github/v/release/sayedalve/Radium-Download-Manager?style=flat-square"></a>
  <a href="https://github.com/sayedalve/Radium-Download-Manager/releases/latest"><img alt="Baixar para Windows" src="https://img.shields.io/badge/Download-Windows%20x64%20(v1.0.0)-0078D4?style=flat-square&logo=windows11"></a>
  <img alt="Linguagem: C++20" src="https://img.shields.io/badge/Language-ISO%20C%2B%2B20-00599C?style=flat-square&logo=c%2B%2B">
  <img alt="Interface: Win32 Nativa" src="https://img.shields.io/badge/UI-Native%20Win32-22c55e?style=flat-square">
  <img alt="Extensão: Manifest V3" src="https://img.shields.io/badge/Extension-Manifest%20V3-eab308?style=flat-square&logo=googlechrome">
  <img alt="Licença: Apache-2.0" src="https://img.shields.io/badge/License-Apache--2.0-blue.svg?style=flat-square">
  <a href="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml"><img alt="Build para Windows" src="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml/badge.svg?branch=main"></a>
</p>

<p align="center">
  <a href="#visão-geral"><b>Visão Geral</b></a> ·
  <a href="#downloads-e-instalação"><b>Downloads</b></a> ·
  <a href="#principais-recursos"><b>Recursos</b></a> ·
  <a href="#temas-claro-e-escuro"><b>Temas</b></a> ·
  <a href="#interface-multilíngue"><b>Idiomas</b></a> ·
  <a href="#capturas-de-tela"><b>Capturas</b></a> ·
  <a href="#testes-de-desempenho"><b>Desempenho</b></a> ·
  <a href="#motor-de-download-de-alto-desempenho"><b>Motor</b></a> ·
  <a href="#downloads-bittorrent"><b>BitTorrent</b></a> ·
  <a href="#pipeline-adaptativo-de-transmissão"><b>Streams</b></a> ·
  <a href="#integração-com-o-navegador"><b>Navegador</b></a> ·
  <a href="#arquitetura-interna"><b>Arquitetura</b></a> ·
  <a href="#testes-e-verificação"><b>Testes</b></a> ·
  <a href="#licença-e-créditos"><b>Licença</b></a>
</p>

<br>

<p align="center">
  <img src="docs/images/readme/radium-main-window.png" alt="Janela principal do Radium Download Manager" width="100%">
  <br>
  <sub>Janela principal do Radium: tema escuro Deep Charcoal, barras de progresso segmentadas em tempo real, telemetria de velocidade e filtros de categorias.</sub>
</p>

<br>

## Visão Geral

O Radium é um gerenciador de downloads ágil e moderno para Windows 11, criado para substituir softwares pesados e repletos de anúncios. Ele une downloads segmentados de alta velocidade à captura prática de vídeos e multiplexação em um só clique.

<table>
  <tr>
    <td width="25%" valign="top">
      <h3>⚡ Velocidade Nativa</h3>
      Sem nada de Electron. Desenvolvido em C++20 puro com Win32 e Direct2D/GDI. Inicialização instantânea e uso mínimo de memória (18 a 22 MB de pico mesmo durante transferências pesadas de arquivos grandes).
    </td>
    <td width="25%" valign="top">
      <h3>🔀 Motor Adaptativo</h3>
      Escala dinâmica de 1 a 32 conexões com algoritmo de divisão por histerese e roubo de trabalho (work-stealing). Registrou tempos de conclusão 16,5% a 19,2% menores que o IDM em testes reais de 500 MB.
    </td>
    <td width="25%" valign="top">
      <h3>🌐 Captura no Navegador</h3>
      Extensão Manifest V3 para Chrome, Brave, Edge e Firefox. Detecta transmissões e mídias usando canais nomeados (Named Pipes) seguros com ACL do usuário, sem expor portas localhost desprotegidas.
    </td>
    <td width="25%" valign="top">
      <h3>🧲 BitTorrent e Streams</h3>
      Motor BitTorrent v1 com suporte a magnet links (BEP 9), anúncios em trackers e árvore de arquivos seletiva. Execução segura fora de processo do N_m3u8DL-RE e FFmpeg em Windows Job Objects isolados.
    </td>
  </tr>
</table>

<br>

## Downloads e Instalação

A versão estável mais recente é a **[v1.0.0](https://github.com/sayedalve/Radium-Download-Manager/releases/tag/v1.0.0)** :

| Pacote | Arquitetura | Formato | Descrição |
| :--- | :---: | :---: | :--- |
| **Instalador Radium (Padrão)** | x64 | [Setup (.exe)](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v1.0.0/Radium-Setup-x64-1.0.0.exe) | Instalador recomendado para Windows 11 / 10 x64 |
| **Radium Portátil Completo** | x64 | [Arquivo ZIP](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v1.0.0/Radium-v1.0.0-Windows-x64-Full.zip) | Pacote portátil contendo ferramentas N_m3u8DL-RE e FFmpeg |
| **Radium Portátil Core** | x64 | [Arquivo ZIP](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v1.0.0/Radium-v1.0.0-Windows-x64-Core.zip) | Pacote básico (ferramentas de fluxo requeridas à parte) |
| **Instalador Core** | x64 | [Setup (.exe)](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v1.0.0/Radium-Setup-x64-Core-1.0.0.exe) | Instalador compacto sem as dependências de stream |
| **Instalador ARM64** | ARM64 | [Setup (.exe)](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v1.0.0/Radium-Setup-ARM64-1.0.0.exe) | Instalador nativo para dispositivos Windows 11 em ARM |
| **Radium Portátil ARM64** | ARM64 | [Arquivo ZIP](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v1.0.0/Radium-v1.0.0-Windows-ARM64-Full.zip) | Pacote portátil com ferramentas completas para ARM64 |
| **Extensão do Navegador** | Multi-navegador | [código da extensão](extension/) | Extensão Manifest V3 para Chrome, Brave, Edge e Firefox |

Checksums de segurança: [SHA256SUMS.txt](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v1.0.0/SHA256SUMS.txt).

> [!IMPORTANT]
> ### 🧩 Instalação da Extensão do Navegador (Carregamento Manual)
> **A extensão do navegador não vem instalada automaticamente pelo instalador.** Você deve baixar os arquivos neste repositório e carregá-los em modo desenvolvedor:
> 1. Baixe ou clone a pasta [`extension/`](extension/) deste repositório (**Code → Download ZIP**).
> 2. Execute o Radium para registrar o Host de Mensagens Nativas no Registro do Windows.
> 3. Nos navegadores Chromium (`chrome://extensions`, `edge://extensions`), ative o **Modo do desenvolvedor**, clique em **Carregar sem compactação** e selecione a pasta `extension`.
> 4. No Firefox, vá em `about:debugging#/runtime/this-firefox`, clique em **Carregar extensão temporária...** e selecione o arquivo `manifest.firefox.json`.
> 5. Se estiver usando a versão portátil, execute antes `scripts\register-native-messaging.ps1`.

<br>

## Temas Claro e Escuro

O Radium suporta de forma completa e nativa tanto o tema Claro (Light) quanto o tema Escuro (Dark):

- **Modo Claro (Light Mode):** Fundo branco nítido (`#FFFFFF`), superfícies elevadas neutras (`#F5F7F9`), detalhes em Azul Céu (`#168BCB`) e tipografia de alto contraste (`#202124`).
- **Modo Escuro (Dark Mode):** Fundo ardósia profundo (`#0F172A` / `#161618`), acabamentos em azul-marinho e o tradicional Vermelho Coral (`#FF4757`) nos acentos de ação.
- **Botão Dedicado na Barra:** Alterne facilmente entre claro e escuro a qualquer momento com um clique no botão de tema da barra principal.
- **Sincronização com o Windows:** Monitoramento contínuo da preferência do sistema operacional (`AppsUseLightTheme`).

<br>

## Interface Multilíngue

Projetado globalmente, o Radium inclui um sistema de internacionalização em C++20 com catálogos locais e funcionamento autônomo sem conexão.

> [!NOTE]
> A documentação do GitHub está disponível em **12 idiomas principais** (acessíveis no topo). Já **o aplicativo Radium para computador oferece suporte a 24 idiomas e alfabetos nativos**!

### Idiomas Disponíveis na Aplicação
O aplicativo de desktop Radium suporta:
Inglês (`en`), Japonês (`ja`), Chinês Simplificado (`zh-CN`), Chinês Tradicional (`zh-TW`), Coreano (`ko`), Espanhol (`es`), Francês (`fr`), Português do Brasil (`pt-BR`), Alemão (`de-DE`), Russo (`ru-RU`), Italiano (`it-IT`), Árabe (`ar-SA` - RTL), além de vietnamita, holandês, polonês, turco, indonésio, persa (RTL), ucraniano, tcheco, romeno, tailandês, sueco e grego.

<br>

## Capturas de Tela

<p align="center">
  <img src="docs/images/readme/radium-main-window.png" alt="Janela principal do Radium" width="100%">
  <br>
  <sub><b>Tela Principal:</b> Painel completo de downloads com visualizador de conexões em tempo real e categorias.</sub>
</p>

<table>
  <tr>
    <td width="50%" valign="top">
      <img src="docs/images/readme/radium-new-download-dialog.jpg" alt="Novo download" width="100%">
      <br>
      <sub><b>Sondagem de Download:</b> Inspeção de faixas e cálculo de espaço em disco.</sub>
    </td>
    <td width="50%" valign="top">
      <img src="docs/images/readme/radium-add-torrent-dialog.jpg" alt="Adicionar torrent" width="100%">
      <br>
      <sub><b>Gerenciador BitTorrent:</b> Seleção de arquivos e verificação de integridade.</sub>
    </td>
  </tr>
  <tr>
    <td colspan="2" valign="top">
      <img src="docs/images/readme/radium-settings-dialog.jpg" alt="Centro de configurações" width="100%">
      <br>
      <sub><b>Centro de Configurações:</b> Troca de idiomas em grafia nativa, temas, limites e roteamento.</sub>
    </td>
  </tr>
</table>

<br>

## Licença e Créditos

Distribuído sob a **Licença Apache 2.0** ([Apache-2.0](LICENSE)).

Componentes de terceiros: [N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) (MIT), [FFmpeg](https://ffmpeg.org/) (LGPL-2.1+ / LGPL-3.0), [Microsoft WIL](https://github.com/microsoft/wil) (MIT), [SQLite](https://www.sqlite.org/) (Domínio Público), [nlohmann/json](https://github.com/nlohmann/json) (MIT).

<br>

<p align="center">
  <sub>Radium Download Manager · Gerenciador de downloads nativo e de alta performance para Windows.</sub><br>
  <sub>Construído com C++20 e Win32. Licenciado sob Apache-2.0.</sub>
</p>
