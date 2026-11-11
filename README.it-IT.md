<p align="center">
  <img src="assets/branding/RDM_Logo.svg" alt="Logo Ufficiale di Radium Download Manager" width="128" height="128">
</p>

<p align="center">
  <picture>
    <img alt="Radium Download Manager" src="https://img.shields.io/badge/RADIUM-DOWNLOAD%20MANAGER-e03131?style=for-the-badge&logo=windows11&logoColor=white" height="42">
  </picture>
</p>

<h1 align="center">Radium Download Manager</h1>

<p align="center">
  <a href="README.md">English</a> · <a href="README.ja.md">日本語</a> · <a href="README.zh-CN.md">简体中文</a> · <a href="README.zh-TW.md">繁體中文</a> · <a href="README.ko.md">한국어</a> · <a href="README.es.md">Español</a> · <a href="README.fr.md">Français</a> · <a href="README.pt-BR.md">Português</a> · <a href="README.de-DE.md">Deutsch</a> · <a href="README.ru-RU.md">Русский</a> · <b>Italiano</b> · <a href="README.ar-SA.md">العربية</a>
</p>

<p align="center">
  <b>Download manager veloce e compatto per Windows 11 con cattura video integrata.</b><br>
  Accelera i download con connessioni multiple e cattura video dai browser supportati (HLS, DASH e stream web).<br>
  Applicazione nativa C++20 e Win32, senza il peso di Electron, con minimo utilizzo di RAM e massima reattività.
</p>

<p align="center">
  <a href="https://github.com/sayedalve/Radium-Download-Manager/releases/latest"><img alt="Ultima release" src="https://img.shields.io/github/v/release/sayedalve/Radium-Download-Manager?style=flat-square"></a>
  <a href="https://github.com/sayedalve/Radium-Download-Manager/releases/latest"><img alt="Scarica per Windows" src="https://img.shields.io/badge/Download-Windows%20x64%20(v1.0.0)-0078D4?style=flat-square&logo=windows11"></a>
  <img alt="Linguaggio: C++20" src="https://img.shields.io/badge/Language-ISO%20C%2B%2B20-00599C?style=flat-square&logo=c%2B%2B">
  <img alt="UI: Win32 Nativa" src="https://img.shields.io/badge/UI-Native%20Win32-22c55e?style=flat-square">
  <img alt="Estensione: Manifest V3" src="https://img.shields.io/badge/Extension-Manifest%20V3-eab308?style=flat-square&logo=googlechrome">
  <img alt="Licenza: Apache-2.0" src="https://img.shields.io/badge/License-Apache--2.0-blue.svg?style=flat-square">
  <a href="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml"><img alt="Build per Windows" src="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml/badge.svg?branch=main"></a>
</p>

<p align="center">
  <a href="#panoramica"><b>Panoramica</b></a> ·
  <a href="#download-e-installazione"><b>Download</b></a> ·
  <a href="#caratteristiche-principali"><b>Caratteristiche</b></a> ·
  <a href="#temi-chiaro-e-scuro"><b>Temi</b></a> ·
  <a href="#interfaccia-multilingue"><b>Lingue</b></a> ·
  <a href="#screenshot"><b>Screenshot</b></a> ·
  <a href="#benchmark-delle-prestazioni"><b>Prestazioni</b></a> ·
  <a href="#motore-di-download-ad-alte-prestazioni"><b>Motore</b></a> ·
  <a href="#download-bittorrent"><b>BitTorrent</b></a> ·
  <a href="#pipeline-di-streaming-adattiva"><b>Streaming</b></a> ·
  <a href="#integrazione-browser"><b>Browser</b></a> ·
  <a href="#architettura-interna"><b>Architettura</b></a> ·
  <a href="#test-e-verifica"><b>Test</b></a> ·
  <a href="#licenza-e-crediti"><b>Licenza</b></a>
</p>

<br>

<p align="center">
  <img src="docs/images/readme/radium-main-window.png" alt="Finestra principale di Radium Download Manager" width="100%">
  <br>
  <sub>Finestra principale di Radium: tema scuro Deep Charcoal, barre di avanzamento segmentate in tempo reale, telemetria della velocità e categorie.</sub>
</p>

<br>

## Panoramica

Radium è un gestore di download reattivo e pulito per Windows 11, sviluppato per offrire un'alternativa moderna e priva di annunci ai software tradizionali. Offre download segmentati ad altissima velocità e intercettazione dei flussi video con muxing immediato.

<table>
  <tr>
    <td width="25%" valign="top">
      <h3>⚡ Velocità Nativa</h3>
      Zero Electron. Scritto interamente in C++20 con Win32 e Direct2D/GDI. Si avvia in millisecondi e mantiene un consumo tipico di 18–22 MB di RAM anche durante trasferimenti impegnativi.
    </td>
    <td width="25%" valign="top">
      <h3>🔀 Motore Adattivo</h3>
      Regolazione dinamica (da 1 a 32 connessioni) con bilanciamento del carico tramite work-stealing. Ha registrato tempi inferiori dal 16,5% al 19,2% rispetto a IDM su download reali da 500 MB.
    </td>
    <td width="25%" valign="top">
      <h3>🌐 Cattura dal Browser</h3>
      Estensione Manifest V3 per Chrome, Brave, Edge e Firefox. Comunica in modo sicuro tramite Named Pipe di Windows protette da ACL, senza aprire porte localhost non protette.
    </td>
    <td width="25%" valign="top">
      <h3>🧲 BitTorrent &amp; Stream</h3>
      Client BitTorrent v1 con metadati magnet (BEP 9), tracker e albero dei file interattivo. Elaborazione video affidabile con N_m3u8DL-RE e FFmpeg in Windows Job Objects isolati.
    </td>
  </tr>
</table>

<br>

## Temi Chiaro e Scuro

Radium offre un supporto nativo di prima classe per i temi chiaro (Light Mode) e scuro (Dark Mode):

- **Modalità Chiara (Light Mode):** Sfondo bianco puro (`#FFFFFF`), superfici secondarie chiare (`#F5F7F9`), accenti blu cielo (`#168BCB`) e leggibilità ottimale con testo scuro (`#202124`).
- **Modalità Scura (Dark Mode):** Sfondo ardesia profondo (`#0F172A` / `#161618`), superfici blu navy e accento rosso corallo classico (`#FF4757`).
- **Pulsante dedicato nella barra:** Un pulsante nella barra degli strumenti principale consente di commutare all'istante tra chiaro e scuro.
- **Sincronizzazione di sistema:** Segue automaticamente il tema impostato su Windows (`AppsUseLightTheme`).

<br>

## Interfaccia Multilingue

Radium è sviluppato per utenti internazionali e funziona interamente offline grazie a cataloghi linguistici JSON locali.

> [!NOTE]
> Il README su GitHub è disponibile in **12 lingue principali** (tramite il selettore in alto). L'**applicazione Radium per desktop supporta già 24 lingue e sistemi di scrittura nativi**!

Lingue supportate nell'applicazione:
Inglese (`en`), Giapponese (`ja`), Cinese Semplificato (`zh-CN`), Cinese Tradizionale (`zh-TW`), Coreano (`ko`), Spagnolo (`es`), Francese (`fr`), Portoghese Brasiliano (`pt-BR`), Tedesco (`de-DE`), Russo (`ru-RU`), Italiano (`it-IT`), Arabo (`ar-SA` - RTL), e inoltre vietnamita, olandese, polacco, turco, indonesiano, persiano (RTL), ucraino, ceco, rumeno, tailandese, svedese e greco.

<br>

## Screenshot

<p align="center">
  <img src="docs/images/readme/radium-main-window.png" alt="Finestra principale di Radium" width="100%">
  <br>
  <sub><b>Finestra Principale:</b> Visualizzatore dei segmenti in tempo reale e statistiche di trasferimento.</sub>
</p>

<table>
  <tr>
    <td width="50%" valign="top">
      <img src="docs/images/readme/radium-new-download-dialog.jpg" alt="Nuovo download" width="100%">
      <br>
      <sub><b>Analisi Preventiva:</b> Controllo dei byte-range e calcolo dello spazio su disco.</sub>
    </td>
    <td width="50%" valign="top">
      <img src="docs/images/readme/radium-add-torrent-dialog.jpg" alt="Aggiungi torrent" width="100%">
      <br>
      <sub><b>Client BitTorrent:</b> Albero dei file per la selezione mirata dei contenuti.</sub>
    </td>
  </tr>
  <tr>
    <td colspan="2" valign="top">
      <img src="docs/images/readme/radium-settings-dialog.jpg" alt="Centro impostazioni" width="100%">
      <br>
      <sub><b>Centro Impostazioni:</b> Scelta della lingua in caratteri nativi, temi, limiti di banda e proxy.</sub>
    </td>
  </tr>
</table>

<br>

## Licenza e Crediti

Radium Download Manager è distribuito sotto licenza **Apache 2.0** ([Apache-2.0](LICENSE)).

Componenti terzi: [N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) (MIT), [FFmpeg](https://ffmpeg.org/) (LGPL-2.1+ / LGPL-3.0), [Microsoft WIL](https://github.com/microsoft/wil) (MIT), [SQLite](https://www.sqlite.org/) (Pubblico Dominio), [nlohmann/json](https://github.com/nlohmann/json) (MIT).

<br>

<p align="center">
  <sub>Radium Download Manager · Download manager nativo, aperto e ad alte prestazioni per Windows.</sub><br>
  <sub>Sviluppato in C++20 e Win32 con licenza Apache-2.0.</sub>
</p>
