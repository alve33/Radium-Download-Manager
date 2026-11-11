<p align="center">
  <img src="assets/branding/RDM_Logo.svg" alt="Offizielles Logo des Radium Download Managers" width="128" height="128">
</p>

<p align="center">
  <picture>
    <img alt="Radium Download Manager" src="https://img.shields.io/badge/RADIUM-DOWNLOAD%20MANAGER-e03131?style=for-the-badge&logo=windows11&logoColor=white" height="42">
  </picture>
</p>

<h1 align="center">Radium Download Manager</h1>

<p align="center">
  <a href="README.md">English</a> · <a href="README.ja.md">日本語</a> · <a href="README.zh-CN.md">简体中文</a> · <a href="README.zh-TW.md">繁體中文</a> · <a href="README.ko.md">한국어</a> · <a href="README.es.md">Español</a> · <a href="README.fr.md">Français</a> · <a href="README.pt-BR.md">Português</a> · <b>Deutsch</b> · <a href="README.ru-RU.md">Русский</a> · <a href="README.it-IT.md">Italiano</a> · <a href="README.ar-SA.md">العربية</a>
</p>

<p align="center">
  <b>Schneller und schlanker Windows 11 Download-Manager mit integrierter Video-Erkennung.</b><br>
  Beschleunigen Sie segmentierte Downloads und erfassen Sie Web-Videos (HLS, DASH und Direkt-Streams) aus modernen Browsern.<br>
  Eine native C++20 &amp; Win32-Applikation ohne Electron-Ballast, mit minimalem Arbeitsspeicherbedarf und ohne störende Hintergrundprozesse.
</p>

<p align="center">
  <a href="https://github.com/sayedalve/Radium-Download-Manager/releases/latest"><img alt="Neueste Version" src="https://img.shields.io/github/v/release/sayedalve/Radium-Download-Manager?style=flat-square"></a>
  <a href="https://github.com/sayedalve/Radium-Download-Manager/releases/latest"><img alt="Download für Windows" src="https://img.shields.io/badge/Download-Windows%20x64%20(v1.0.0)-0078D4?style=flat-square&logo=windows11"></a>
  <img alt="Sprache: C++20" src="https://img.shields.io/badge/Language-ISO%20C%2B%2B20-00599C?style=flat-square&logo=c%2B%2B">
  <img alt="UI: Natives Win32" src="https://img.shields.io/badge/UI-Native%20Win32-22c55e?style=flat-square">
  <img alt="Erweiterung: Manifest V3" src="https://img.shields.io/badge/Extension-Manifest%20V3-eab308?style=flat-square&logo=googlechrome">
  <img alt="Lizenz: Apache-2.0" src="https://img.shields.io/badge/License-Apache--2.0-blue.svg?style=flat-square">
  <a href="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml"><img alt="Windows Release Build" src="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml/badge.svg?branch=main"></a>
</p>

<p align="center">
  <a href="#überblick"><b>Überblick</b></a> ·
  <a href="#downloads-und-installation"><b>Downloads</b></a> ·
  <a href="#hauptfunktionen"><b>Funktionen</b></a> ·
  <a href="#helles-und-dunkles-design"><b>Designs</b></a> ·
  <a href="#mehrsprachige-benutzeroberfläche"><b>Sprachen</b></a> ·
  <a href="#bildschirmfotos"><b>Screenshots</b></a> ·
  <a href="#leistungs-benchmarks"><b>Leistung</b></a> ·
  <a href="#download-engine"><b>Engine</b></a> ·
  <a href="#bittorrent-downloads"><b>BitTorrent</b></a> ·
  <a href="#stream-verarbeitung"><b>Streams</b></a> ·
  <a href="#browser-integration"><b>Browser</b></a> ·
  <a href="#systemarchitektur"><b>Architektur</b></a> ·
  <a href="#tests-und-verifikation"><b>Tests</b></a> ·
  <a href="#lizenz-und-danksagungen"><b>Lizenz</b></a>
</p>

<br>

<p align="center">
  <img src="docs/images/readme/radium-main-window.png" alt="Hauptfenster des Radium Download Managers" width="100%">
  <br>
  <sub>Das Hauptfenster von Radium: Dunkles Anthrazit-Design, Live-Segmentanzeige, Übertragungsstatistiken und Kategoriefilter.</sub>
</p>

<br>

## Überblick

Radium ist ein ressourcenschonender, blitzschneller Download-Manager für Windows 11, der als zeitgemäßer Ersatz für überladene und werbefinanzierte Altprogramme entwickelt wurde. Er vereint Multiverbindungs-Downloads mit 1-Klick-Videoerfassung und Stream-Multiplexing.

<table>
  <tr>
    <td width="25%" valign="top">
      <h3>⚡ Native Geschwindigkeit</h3>
      Kein Electron. Ausschließlich in ISO C++20 mit Win32 und Direct2D/GDI geschrieben. Startet in Millisekunden und belegt selbst bei Multi-Gigabyte-Downloads nur 18 bis 22 MB RAM.
    </td>
    <td width="25%" valign="top">
      <h3>🔀 Adaptive Engine</h3>
      Dynamische Skalierung (1 bis 32 Verbindungen) mit intelligentem Work-Stealing. Im 500-MB-Praxistest war Radium 16,5% bis 19,2% schneller fertig als der Internet Download Manager (IDM).
    </td>
    <td width="25%" valign="top">
      <h3>🌐 Browser-Erfassung</h3>
      Manifest V3-Erweiterung für Chrome, Brave, Edge und Firefox. Kommuniziert abhörsicher über benannte Windows-Pipes (Named Pipes) mit Benutzer-ACLs – ohne offene Localhost-Ports.
    </td>
    <td width="25%" valign="top">
      <h3>🧲 BitTorrent &amp; Streams</h3>
      Integrierte BitTorrent v1-Engine mit Magnet-Link-Auflösung (BEP 9), Tracker-Verwaltung und Dateiauswahl. Externe Stream-Verarbeitung via N_m3u8DL-RE und FFmpeg in isolierten Windows Job Objects.
    </td>
  </tr>
</table>

<br>

## Helles und Dunkles Design

Radium bietet erstklassige visuelle Unterstützung für Hell- und Dunkelmodi (Light und Dark Mode):

- **Heller Modus (Light Mode):** Klares weißes Interface (`#FFFFFF`), dezente helle Oberflächen (`#F5F7F9`), Akzente in Himmelblau (`#168BCB`) und erstklassige Lesbarkeit (`#202124`).
- **Dunkler Modus (Dark Mode):** Elegantes Schiefergrau (`#0F172A` / `#161618`), Marineblau-Flächen und der vertraute Korallenrot-Akzent (`#FF4757`).
- **Umschalt-Schaltfläche:** Über eine separate Schaltfläche in der Hauptsymbolleiste lässt sich die Ansicht jederzeit mit einem Klick umschalten.
- **Automatische Synchronisation:** Erkennt das Windows-Systemdesign (`AppsUseLightTheme`) und passt sich sofort ohne Neustart an.

<br>

## Mehrsprachige Benutzeroberfläche

Radium wurde für den weltweiten Einsatz konzipiert und arbeitet vollständig offline mit lokalen Sprachdateien.

> [!NOTE]
> Die GitHub-Dokumentation ist in **12 Kernsprachen** übersetzt (siehe Sprachauswahl oben). Das **Radium-Desktop-Programm unterstützt bereits 24 native Sprachen und Schriftsysteme**!

Unterstützte Oberflächensprachen in der App:
Englisch (`en`), Japanisch (`ja`), Vereinfachtes Chinesisch (`zh-CN`), Traditionelles Chinesisch (`zh-TW`), Koreanisch (`ko`), Spanisch (`es`), Französisch (`fr`), Brasilianisches Portugiesisch (`pt-BR`), Deutsch (`de-DE`), Russisch (`ru-RU`), Italienisch (`it-IT`), Arabisch (`ar-SA` - RTL), Vietnamesisch, Niederländisch, Polnisch, Türkisch, Indonesisch, Persisch (RTL), Ukrainisch, Tschechisch, Rumänisch, Thailändisch, Schwedisch und Griechisch.

<br>

## Bildschirmfotos

<p align="center">
  <img src="docs/images/readme/radium-main-window.png" alt="Hauptansicht" width="100%">
  <br>
  <sub><b>Hauptfenster:</b> Übersichtliches Dashboard mit grafischer Segmentverteilung und Telemetriedaten.</sub>
</p>

<table>
  <tr>
    <td width="50%" valign="top">
      <img src="docs/images/readme/radium-new-download-dialog.jpg" alt="Neuer Download" width="100%">
      <br>
      <sub><b>Vorab-Prüfung:</b> Range-Validierung und Speicherplatzberechnung.</sub>
    </td>
    <td width="50%" valign="top">
      <img src="docs/images/readme/radium-add-torrent-dialog.jpg" alt="Torrent hinzufügen" width="100%">
      <br>
      <sub><b>BitTorrent:</b> Selektiver Verzeichnisbaum und Chunk-Überprüfung.</sub>
    </td>
  </tr>
  <tr>
    <td colspan="2" valign="top">
      <img src="docs/images/readme/radium-settings-dialog.jpg" alt="Einstellungen" width="100%">
      <br>
      <sub><b>Einstellungen:</b> Sprachauswahl in nativer Schriftart, Design-Präferenzen und Verbindungslimits.</sub>
    </td>
  </tr>
</table>

<br>

## Lizenz und Danksagungen

Lizenziert unter der **Apache License 2.0** ([Apache-2.0](LICENSE)).

Drittanbieterkomponenten: [N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) (MIT), [FFmpeg](https://ffmpeg.org/) (LGPL-2.1+ / LGPL-3.0), [Microsoft WIL](https://github.com/microsoft/wil) (MIT), [SQLite](https://www.sqlite.org/) (Public Domain), [nlohmann/json](https://github.com/nlohmann/json) (MIT).

<br>

<p align="center">
  <sub>Radium Download Manager · Hochperformanter, nativer Open-Source-Download-Manager für Windows.</sub><br>
  <sub>Erstellt mit C++20 und Win32. Lizenziert unter Apache-2.0.</sub>
</p>
