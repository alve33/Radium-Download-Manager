<p align="center">
  <img src="assets/branding/RDM_Logo.svg" alt="Официальный логотип Radium Download Manager" width="128" height="128">
</p>

<p align="center">
  <picture>
    <img alt="Radium Download Manager" src="https://img.shields.io/badge/RADIUM-DOWNLOAD%20MANAGER-e03131?style=for-the-badge&logo=windows11&logoColor=white" height="42">
  </picture>
</p>

<h1 align="center">Radium Download Manager</h1>

<p align="center">
  <a href="README.md">English</a> · <a href="README.ja.md">日本語</a> · <a href="README.zh-CN.md">简体中文</a> · <a href="README.zh-TW.md">繁體中文</a> · <a href="README.ko.md">한국어</a> · <a href="README.es.md">Español</a> · <a href="README.fr.md">Français</a> · <a href="README.pt-BR.md">Português</a> · <a href="README.de-DE.md">Deutsch</a> · <b>Русский</b> · <a href="README.it-IT.md">Italiano</a> · <a href="README.ar-SA.md">العربية</a>
</p>

<p align="center">
  <b>Быстрый и удобный менеджер загрузок для Windows 11 со встроенным захватом видео.</b><br>
  Ускоряйте многопоточные загрузки файлов и перехватывайте видео из популярных браузеров (HLS, DASH и веб-потоки).<br>
  Нативное приложение на ISO C++20 и Win32: без громоздкого Electron, с минимальным потреблением ОЗУ и без фонового шума.
</p>

<p align="center">
  <a href="https://github.com/sayedalve/Radium-Download-Manager/releases/latest"><img alt="Последний релиз" src="https://img.shields.io/github/v/release/sayedalve/Radium-Download-Manager?style=flat-square"></a>
  <a href="https://github.com/sayedalve/Radium-Download-Manager/releases/latest"><img alt="Скачать для Windows" src="https://img.shields.io/badge/Download-Windows%20x64%20(v1.0.0)-0078D4?style=flat-square&logo=windows11"></a>
  <img alt="Язык: C++20" src="https://img.shields.io/badge/Language-ISO%20C%2B%2B20-00599C?style=flat-square&logo=c%2B%2B">
  <img alt="Интерфейс: Win32" src="https://img.shields.io/badge/UI-Native%20Win32-22c55e?style=flat-square">
  <img alt="Расширение: Manifest V3" src="https://img.shields.io/badge/Extension-Manifest%20V3-eab308?style=flat-square&logo=googlechrome">
  <img alt="Лицензия: Apache-2.0" src="https://img.shields.io/badge/License-Apache--2.0-blue.svg?style=flat-square">
  <a href="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml"><img alt="Сборка для Windows" src="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml/badge.svg?branch=main"></a>
</p>

<p align="center">
  <a href="#краткий-обзор"><b>Обзор</b></a> ·
  <a href="#загрузка-и-установка"><b>Загрузка</b></a> ·
  <a href="#ключевые-возможности"><b>Возможности</b></a> ·
  <a href="#светлая-и-тёмная-темы"><b>Темы</b></a> ·
  <a href="#многоязычный-интерфейс"><b>Языки</b></a> ·
  <a href="#скриншоты"><b>Скриншоты</b></a> ·
  <a href="#тесты-производительности"><b>Тесты</b></a> ·
  <a href="#высокопроизводительный-движок-загрузки"><b>Движок</b></a> ·
  <a href="#загрузки-bittorrent"><b>BitTorrent</b></a> ·
  <a href="#адаптивная-обработка-видеопотоков"><b>Потоки</b></a> ·
  <a href="#интеграция-с-браузерами"><b>Браузеры</b></a> ·
  <a href="#архитектура"><b>Архитектура</b></a> ·
  <a href="#тестирование-и-проверка"><b>Проверка</b></a> ·
  <a href="#лицензия-и-благодарности"><b>Лицензия</b></a>
</p>

<br>

<p align="center">
  <img src="docs/images/readme/radium-main-window.png" alt="Главное окно Radium Download Manager" width="100%">
  <br>
  <sub>Главное окно Radium: тёмная тема Deep Charcoal, сегментированные индикаторы прогресса, телеметрия скорости и фильтры по категориям.</sub>
</p>

<br>

## Краткий обзор

Radium — это легковесный и производительный менеджер загрузок для Windows 11, созданный в качестве чистой и быстрой альтернативы устаревшим и перегруженным рекламой программам. Он объединяет многопоточное скачивание файлов, перехват видеопотоков в один клик и автоматический ремультиплексинг.

<table>
  <tr>
    <td width="25%" valign="top">
      <h3>⚡ Нативная скорость</h3>
      Никакого Electron. Чистый C++20, Win32 и Direct2D/GDI. Запуск за миллисекунды и пиковое потребление всего 18–22 МБ ОЗУ даже во время загрузки гигабайтных файлов.
    </td>
    <td width="25%" valign="top">
      <h3>🔀 Адаптивный движок</h3>
      Динамическое масштабирование соединений (от 1 до 32) с алгоритмом перехвата незавершённых сегментов (work-stealing). В тестах на 500 МБ показал время завершения на 16.5%–19.2% быстрее IDM.
    </td>
    <td width="25%" valign="top">
      <h3>🌐 Перехват в браузере</h3>
      Расширение Manifest V3 для Chrome, Brave, Edge и Firefox. Безопасное взаимодействие через защищённые именованные каналы Windows (Named Pipes) с пользовательскими ACL — без открытых портов localhost.
    </td>
    <td width="25%" valign="top">
      <h3>🧲 BitTorrent и стриминг</h3>
      Встроенный движок BitTorrent v1 с обменом метаданными (BEP 9), опросом трекеров и выборочной загрузкой файлов. Изолированная обработка потоков через N_m3u8DL-RE и FFmpeg в Windows Job Objects.
    </td>
  </tr>
</table>

<br>

## Светлая и тёмная темы

Radium безупречно интегрируется в Windows 11, предоставляя первоклассную поддержку светлого и тёмного оформления:

- **Светлый режим (Light Mode):** Чистый белый холст (`#FFFFFF`), нейтральные приподнятые поверхности (`#F5F7F9`), небесно-голубой акцент (`#168BCB`) и контрастный угольный текст (`#202124`).
- **Тёмный режим (Dark Mode):** Глубокий оттенок сланца (`#0F172A` / `#161618`), темно-синие панели (`#1E293B`) и фирменный кораллово-красный цвет (`#FF4757`).
- **Кнопка быстрого переключения:** Отдельная кнопка на главной панели инструментов позволяет мгновенно менять оформление в один клик.
- **Синхронизация с Windows:** Автоматическое отслеживание системного параметра Windows (`AppsUseLightTheme`).

<br>

## Многоязычный интерфейс

Radium изначально ориентирован на международное сообщество и работает полностью автономно с локальными языковыми каталогами.

> [!NOTE]
> Документация GitHub переведена на **12 ключевых языков** (доступны в верхнем селекторе). При этом **само десктопное приложение Radium уже включает интерфейс на 24 языках с их родной письменностью**!

Языки в приложении Radium:
Английский (`en`), Японский (`ja`), Упрощённый китайский (`zh-CN`), Традиционный китайский (`zh-TW`), Корейский (`ko`), Испанский (`es`), Французский (`fr`), Бразильский португальский (`pt-BR`), Немецкий (`de-DE`), Русский (`ru-RU`), Итальянский (`it-IT`), Арабский (`ar-SA` - RTL), а также вьетнамский, нидерландский, польский, турецкий, индонезийский, персидский (RTL), украинский, чешский, румынский, тайский, шведский и греческий.

<br>

## Скриншоты

<p align="center">
  <img src="docs/images/readme/radium-main-window.png" alt="Главное окно Radium" width="100%">
  <br>
  <sub><b>Главное окно:</b> Визуализация многопоточных сегментов в реальном времени и телеметрия канала.</sub>
</p>

<table>
  <tr>
    <td width="50%" valign="top">
      <img src="docs/images/readme/radium-new-download-dialog.jpg" alt="Диалог новой загрузки" width="100%">
      <br>
      <sub><b>Предварительная проверка:</b> Проверка поддержки диапазонов сервером и расчёт свободного места.</sub>
    </td>
    <td width="50%" valign="top">
      <img src="docs/images/readme/radium-add-torrent-dialog.jpg" alt="Диалог добавления торрента" width="100%">
      <br>
      <sub><b>Менеджер BitTorrent:</b> Дерево файлов с выборочной загрузкой и проверка целостности частей.</sub>
    </td>
  </tr>
  <tr>
    <td colspan="2" valign="top">
      <img src="docs/images/readme/radium-settings-dialog.jpg" alt="Центр настроек" width="100%">
      <br>
      <sub><b>Центр настроек:</b> Выбор языка в родном начертании, темы оформления, лимиты скорости и прокси.</sub>
    </td>
  </tr>
</table>

<br>

## Лицензия и благодарности

Radium Download Manager распространяется под лицензией **Apache 2.0** ([Apache-2.0](LICENSE)).

Сторонние компоненты: [N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) (MIT), [FFmpeg](https://ffmpeg.org/) (LGPL-2.1+ / LGPL-3.0), [Microsoft WIL](https://github.com/microsoft/wil) (MIT), [SQLite](https://www.sqlite.org/) (Public Domain), [nlohmann/json](https://github.com/nlohmann/json) (MIT).

<br>

<p align="center">
  <sub>Radium Download Manager · Быстрый нативный менеджер загрузок с открытым исходным кодом для Windows.</sub><br>
  <sub>Создан на C++20 и Win32. Лицензия Apache-2.0.</sub>
</p>
