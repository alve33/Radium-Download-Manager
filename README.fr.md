<p align="center">
  <img src="assets/branding/RDM_Logo.svg" alt="Logo officiel de Radium Download Manager" width="128" height="128">
</p>

<p align="center">
  <picture>
    <img alt="Radium Download Manager" src="https://img.shields.io/badge/RADIUM-DOWNLOAD%20MANAGER-e03131?style=for-the-badge&logo=windows11&logoColor=white" height="42">
  </picture>
</p>

<h1 align="center">Radium Download Manager</h1>

<p align="center">
  <a href="README.md">English</a> · <a href="README.ja.md">日本語</a> · <a href="README.zh-CN.md">简体中文</a> · <a href="README.zh-TW.md">繁體中文</a> · <a href="README.ko.md">한국어</a> · <a href="README.es.md">Español</a> · <b>Français</b> · <a href="README.pt-BR.md">Português</a> · <a href="README.de-DE.md">Deutsch</a> · <a href="README.ru-RU.md">Русский</a> · <a href="README.it-IT.md">Italiano</a> · <a href="README.ar-SA.md">العربية</a>
</p>

<p align="center">
  <b>Gestionnaire de téléchargements rapide et fluide pour Windows 11 avec capture vidéo intégrée.</b><br>
  Accélérez les téléchargements multiconnexions et capturez des vidéos depuis vos navigateurs (HLS, DASH et flux web).<br>
  Application native C++20 et Win32 ultra-légère, sans la surcharge d'Electron, avec un minimum de RAM et zéro nuisance.
</p>

<p align="center">
  <a href="https://github.com/sayedalve/Radium-Download-Manager/releases/latest"><img alt="Dernière version" src="https://img.shields.io/github/v/release/sayedalve/Radium-Download-Manager?style=flat-square"></a>
  <a href="https://github.com/sayedalve/Radium-Download-Manager/releases/latest"><img alt="Télécharger pour Windows" src="https://img.shields.io/badge/Download-Windows%20x64%20(v1.0.0)-0078D4?style=flat-square&logo=windows11"></a>
  <img alt="Langage: C++20" src="https://img.shields.io/badge/Language-ISO%20C%2B%2B20-00599C?style=flat-square&logo=c%2B%2B">
  <img alt="Interface: Win32 native" src="https://img.shields.io/badge/UI-Native%20Win32-22c55e?style=flat-square">
  <img alt="Extension: Manifest V3" src="https://img.shields.io/badge/Extension-Manifest%20V3-eab308?style=flat-square&logo=googlechrome">
  <img alt="Licence: Apache-2.0" src="https://img.shields.io/badge/License-Apache--2.0-blue.svg?style=flat-square">
  <a href="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml"><img alt="Build Windows Release" src="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml/badge.svg?branch=main"></a>
</p>

<p align="center">
  <a href="#vue-densemble"><b>Présentation</b></a> ·
  <a href="#téléchargements-et-installation"><b>Téléchargements</b></a> ·
  <a href="#fonctionnalités-clés"><b>Fonctionnalités</b></a> ·
  <a href="#thèmes-clair-et-sombre"><b>Thèmes</b></a> ·
  <a href="#interface-multilingue"><b>Langues</b></a> ·
  <a href="#captures-décran"><b>Captures</b></a> ·
  <a href="#benchmarks-de-performance"><b>Performances</b></a> ·
  <a href="#moteur-de-téléchargement-haute-performance"><b>Moteur</b></a> ·
  <a href="#téléchargements-bittorrent"><b>BitTorrent</b></a> ·
  <a href="#pipeline-de-flux-adaptatif"><b>Flux Multimédia</b></a> ·
  <a href="#intégration-du-navigateur"><b>Navigateur</b></a> ·
  <a href="#architecture-interne"><b>Architecture</b></a> ·
  <a href="#tests-et-vérification"><b>Tests</b></a> ·
  <a href="#licence-et-crédits"><b>Licence</b></a>
</p>

<br>

<p align="center">
  <img src="docs/images/readme/radium-main-window.png" alt="Fenêtre principale de Radium Download Manager" width="100%">
  <br>
  <sub>Fenêtre principale de Radium : thème sombre Deep Charcoal, jauges de progression segmentées en direct, télémétrie de vitesse et filtres de catégories.</sub>
</p>

<br>

## Vue d'ensemble

Radium est un gestionnaire de téléchargements moderne et léger pour Windows 11, conçu pour remplacer les logiciels encombrants et saturés de publicités. Il associe des téléchargements segmentés ultra-rapides à la capture vidéo en un clic et au multiplexage de flux.

<table>
  <tr>
    <td width="25%" valign="top">
      <h3>⚡ Rapidité Native</h3>
      Zéro surcharge Electron. Conçu exclusivement en ISO C++20 avec Win32 et Direct2D/GDI. Démarrage instantané et empreinte RAM minime (18 à 22 Mo en pointe lors de transferts de plusieurs gigaoctets).
    </td>
    <td width="25%" valign="top">
      <h3>🔀 Moteur Adaptatif</h3>
      Mise à l'échelle dynamique (1 à 32 connexions) avec vol de travail adaptatif (work-stealing). Temps de téléchargement réduit de 16,5% à 19,2% par rapport à IDM sur des transferts réels de 500 Mo.
    </td>
    <td width="25%" valign="top">
      <h3>🌐 Capture Navigateur</h3>
      Extension Manifest V3 pour Chrome, Brave, Edge et Firefox. Détection sécurisée des flux via des canaux nommés Windows (Named Pipes) protégés par ACL utilisateur, sans port localhost non authentifié.
    </td>
    <td width="25%" valign="top">
      <h3>🧲 BitTorrent &amp; Flux</h3>
      Moteur BitTorrent v1 intégré avec support des liens magnet (BEP 9), annonces de trackers et arborescence de fichiers interactive. Traitement externe de flux via N_m3u8DL-RE et FFmpeg dans des Windows Job Objects isolés.
    </td>
  </tr>
</table>

<br>

## Téléchargements et Installation

La version stable la plus récente est **[v1.0.0](https://github.com/sayedalve/Radium-Download-Manager/releases/tag/v1.0.0)** :

| Paquet | Architecture | Format | Description |
| :--- | :---: | :---: | :--- |
| **Installeur Radium (Standard)** | x64 | [Setup (.exe)](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v1.0.0/Radium-Setup-x64-1.0.0.exe) | Installeur recommandé pour Windows 11 / 10 x64 |
| **Radium Portable Complet** | x64 | [Archive ZIP](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v1.0.0/Radium-v1.0.0-Windows-x64-Full.zip) | Version portable intégrant N_m3u8DL-RE et FFmpeg |
| **Radium Portable Core** | x64 | [Archive ZIP](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v1.0.0/Radium-v1.0.0-Windows-x64-Core.zip) | Version portable légère (outils de flux requis séparément) |
| **Installeur Core** | x64 | [Setup (.exe)](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v1.0.0/Radium-Setup-x64-Core-1.0.0.exe) | Installeur sans binaires externes de flux |
| **Installeur ARM64** | ARM64 | [Setup (.exe)](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v1.0.0/Radium-Setup-ARM64-1.0.0.exe) | Installeur natif pour Windows 11 sur ARM |
| **Radium Portable Complet (ARM64)** | ARM64 | [Archive ZIP](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v1.0.0/Radium-v1.0.0-Windows-ARM64-Full.zip) | Version portable avec outils ARM64 |
| **Extension de navigateur** | Multi-navigateur | [Code source](extension/) | Extension Manifest V3 pour Chrome, Edge, Brave, Firefox |

Sommes de contrôle de sécurité : [SHA256SUMS.txt](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v1.0.0/SHA256SUMS.txt).

> [!IMPORTANT]
> ### 🧩 Configuration de l'extension de navigateur (Étape manuelle requise)
> **L'extension n'est pas intégrée automatiquement dans l'installeur.** Vous devez charger les fichiers manuellement :
> 1. Téléchargez ou clonez le dossier [`extension/`](extension/) depuis ce dépôt.
> 2. Lancez Radium pour enregistrer l'hôte de messagerie native dans le registre Windows.
> 3. Ouvrez la gestion des extensions de votre navigateur Chromium (`chrome://extensions`, `edge://extensions`), activez le **Mode développeur**, puis cliquez sur **Charger l'extension non empaquetée** et sélectionnez le dossier `extension`.
> 4. Pour Firefox, ouvrez `about:debugging#/runtime/this-firefox`, cliquez sur **Charger un module temporaire...** et choisissez le fichier `manifest.firefox.json`.
> 5. Avec la version portable, exécutez `scripts\register-native-messaging.ps1` au préalable.

<br>

## Thèmes Clair et Sombre

Radium s'intègre harmonieusement à Windows 11 en offrant une prise en charge native des thèmes clair et sombre :

- **Mode Clair (Light Mode) :** Toile blanche lumineuse (`#FFFFFF`), surfaces neutres (`#F5F7F9`), accents Bleu Ciel (`#168BCB`) et contraste de texte idéal pour le jour (`#202124`).
- **Mode Sombre (Dark Mode) :** Palette ardoise sombre (`#0F172A` / `#161618`), accent Rouge Corail iconique (`#FF4757`) et violet céleste pour les flux (`#8B5CF6`).
- **Bouton d'accès direct :** Un bouton dédié dans la barre d'outils permet de basculer instantanément d'un clic entre le mode clair et le mode sombre avec mémorisation automatique.
- **Synchronisation système :** Détection automatique du paramètre Windows (`AppsUseLightTheme`).

<br>

## Interface Multilingue

Radium est conçu pour les utilisateurs du monde entier grâce à son moteur d'internationalisation natif hors ligne.

> [!NOTE]
> La documentation GitHub est proposée dans **12 langues principales** (via le sélecteur ci-dessus). L'application de bureau Radium propose quant à elle **24 langues et écritures natives** intégrées !

### Langues prises en charge dans l'application
L'application Radium intègre nativement les langues suivantes :
- **Anglais (English)** (`en`) — Langue de référence
- **Japonais (日本語)** (`ja`), **Chinois simplifié (简体中文)** (`zh-CN`), **Chinois traditionnel (繁體中文)** (`zh-TW`), **Coréen (한국어)** (`ko`), **Espagnol (Español)** (`es`), **Français (Français)** (`fr`), **Portugais brésilien (Português - Brasil)** (`pt-BR`), **Allemand (Deutsch)** (`de-DE`), **Russe (Русский)** (`ru-RU`), **Italien (Italiano)** (`it-IT`), **Arabe (العربية - RTL)** (`ar-SA`).
- Ainsi que le vietnamien, le néerlandais, le polonais, le turc, l'indonésien, le persan (RTL), l'ukrainien, le tchèque, le roumain, le thaï, le suédois et le grec.

<br>

## Captures d'écran

<p align="center">
  <img src="docs/images/readme/radium-main-window.png" alt="Interface principale de Radium" width="100%">
  <br>
  <sub><b>Fenêtre Principale :</b> Tableau de bord avec suivi en temps réel des segments et de la bande passante.</sub>
</p>

<table>
  <tr>
    <td width="50%" valign="top">
      <img src="docs/images/readme/radium-new-download-dialog.jpg" alt="Fenêtre de nouveau téléchargement" width="100%">
      <br>
      <sub><b>Sonde Préalable :</b> Analyse des en-têtes et calcul de l'espace disque.</sub>
    </td>
    <td width="50%" valign="top">
      <img src="docs/images/readme/radium-add-torrent-dialog.jpg" alt="Fenêtre d'ajout de torrent" width="100%">
      <br>
      <sub><b>Gestion BitTorrent :</b> Arborescence sélective et vérification de hachage.</sub>
    </td>
  </tr>
  <tr>
    <td colspan="2" valign="top">
      <img src="docs/images/readme/radium-settings-dialog.jpg" alt="Centre de configuration" width="100%">
      <br>
      <sub><b>Centre de Configuration :</b> Sélection des langues dans leur écriture d'origine, gestion des thèmes et réglages réseau.</sub>
    </td>
  </tr>
</table>

<br>

## Licence et Crédits

Radium Download Manager est distribué sous la **Licence Apache 2.0** ([Apache-2.0](LICENSE)).

Composants tiers : [N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) (MIT), [FFmpeg](https://ffmpeg.org/) (LGPL-2.1+ / LGPL-3.0), [Microsoft WIL](https://github.com/microsoft/wil) (MIT), [SQLite](https://www.sqlite.org/) (Domaine public), [nlohmann/json](https://github.com/nlohmann/json) (MIT).

<br>

<p align="center">
  <sub>Radium Download Manager · Gestionnaire de téléchargements natif et haute performance pour Windows.</sub><br>
  <sub>Développé en C++20 et Win32 sous licence Apache-2.0.</sub>
</p>
