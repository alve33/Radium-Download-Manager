<div dir="rtl">

<p align="center">
  <img src="assets/branding/RDM_Logo.svg" alt="الشعار الرسمي لبرنامج Radium Download Manager" width="128" height="128">
</p>

<p align="center">
  <picture>
    <img alt="Radium Download Manager" src="https://img.shields.io/badge/RADIUM-DOWNLOAD%20MANAGER-e03131?style=for-the-badge&logo=windows11&logoColor=white" height="42">
  </picture>
</p>

<h1 align="center">Radium Download Manager</h1>

<p align="center">
  <a href="README.md">English</a> · <a href="README.ja.md">日本語</a> · <a href="README.zh-CN.md">简体中文</a> · <a href="README.zh-TW.md">繁體中文</a> · <a href="README.ko.md">한국어</a> · <a href="README.es.md">Español</a> · <a href="README.fr.md">Français</a> · <a href="README.pt-BR.md">Português</a> · <a href="README.de-DE.md">Deutsch</a> · <a href="README.ru-RU.md">Русский</a> · <a href="README.it-IT.md">Italiano</a> · <b>العربية</b>
</p>

<p align="center">
  <b>مدير تنزيل سريع وخفيف لنظام Windows 11 مع إمكانية مدمجة لالتقاط الفيديو.</b><br>
  تسريع التنزيلات متعددة الاتصالات والتقاط الفيديو من المتصفحات المدعومة (HLS وDASH وبث الويب).<br>
  تطبيق أصلي مبني بـ ISO C++20 وWin32: خالٍ تماماً من ثقل Electron، باستهلاك ضئيل للذاكرة وهدوء تام.
</p>

<p align="center">
  <a href="https://github.com/sayedalve/Radium-Download-Manager/releases/latest"><img alt="أحدث إصدار" src="https://img.shields.io/github/v/release/sayedalve/Radium-Download-Manager?style=flat-square"></a>
  <a href="https://github.com/sayedalve/Radium-Download-Manager/releases/latest"><img alt="تحميل لنظام Windows" src="https://img.shields.io/badge/Download-Windows%20x64%20(v1.0.0)-0078D4?style=flat-square&logo=windows11"></a>
  <img alt="لغة البرمجة: C++20" src="https://img.shields.io/badge/Language-ISO%20C%2B%2B20-00599C?style=flat-square&logo=c%2B%2B">
  <img alt="الواجهة: Win32 أصلية" src="https://img.shields.io/badge/UI-Native%20Win32-22c55e?style=flat-square">
  <img alt="الإضافة: Manifest V3" src="https://img.shields.io/badge/Extension-Manifest%20V3-eab308?style=flat-square&logo=googlechrome">
  <img alt="الترخيص: Apache-2.0" src="https://img.shields.io/badge/License-Apache--2.0-blue.svg?style=flat-square">
  <a href="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml"><img alt="بناء Windows" src="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml/badge.svg?branch=main"></a>
</p>

<p align="center">
  <a href="#نظرة-عامة"><b>نظرة عامة</b></a> ·
  <a href="#التنزيل-والتثبيت"><b>التحميل</b></a> ·
  <a href="#الميزات-الرئيسية"><b>الميزات</b></a> ·
  <a href="#السمتان-الفاتحة-والداكنة"><b>المظهر</b></a> ·
  <a href="#الواجهة-متعددة-اللغات"><b>اللغات</b></a> ·
  <a href="#لقطات-الشاشة"><b>صور البرنامج</b></a> ·
  <a href="#اختبارات-الأداء"><b>الأداء</b></a> ·
  <a href="#محرك-التنزيل-عالي-الكفاءة"><b>المحرك</b></a> ·
  <a href="#تنزيلات-bittorrent"><b>BitTorrent</b></a> ·
  <a href="#معالجة-بث-الفيديو"><b>البث</b></a> ·
  <a href="#التكامل-مع-المتصفح"><b>المتصفح</b></a> ·
  <a href="#البنية-التقنية"><b>البنية</b></a> ·
  <a href="#الاختبار-والتحقق"><b>الاختبار</b></a> ·
  <a href="#الترخيص-والشكر"><b>الترخيص</b></a>
</p>

<br>

<p align="center">
  <img src="docs/images/readme/radium-main-window.png" alt="النافذة الرئيسية لبرنامج Radium Download Manager" width="100%">
  <br>
  <sub>نافذة Radium الرئيسية: مظهر داكن، أشرطة تقدم لحظية للقطع المقسمة، ومؤشرات للسرعة والتصنيفات.</sub>
</p>

<br>

## نظرة عامة

برنامج Radium هو مدير تنزيلات سريع وخفيف لنظام Windows 11، صُمم ليكون بديلاً حديثاً ونظيفاً للبرامج القديمة المليئة بالإعلانات. يجمع بين التنزيل المقسم متعدد الاتصالات والتقاط الفيديو من المتصفح بنقرة واحدة.

<table>
  <tr>
    <td width="25%" valign="top">
      <h3>⚡ سرعة أصلية</h3>
      خالٍ من إطار Electron تماماً. مكتوب بلغة C++20 مع واجهة Win32 وDirect2D/GDI. يعمل فورياً ويستهلك فقط 18–22 ميغابايت من الذاكرة حتى أثناء التنزيل عالي السرعة.
    </td>
    <td width="25%" valign="top">
      <h3>🔀 محرك تكيفي</h3>
      توسيع تلقائي للاتصالات (من 1 إلى 32) مع خوارزمية ذكية لاقتسام الأجزاء المتبقية. حقق سرعة إنهاء أسرع بنسبة 16.5% إلى 19.2% مقارنة بـ IDM في اختبارات 500 ميغابايت الحقيقية.
    </td>
    <td width="25%" valign="top">
      <h3>🌐 التقاط المتصفح</h3>
      إضافة Manifest V3 لمتصفحات Chrome وBrave وEdge وFirefox. تواصل آمن عبر قنوات Windows المسماة (Named Pipes) المحمية بصلاحيات المستخدم، ودون فتح منافذ localhost غير آمنة.
    </td>
    <td width="25%" valign="top">
      <h3>🧲 تورنت وبث مباشر</h3>
      محرك BitTorrent v1 مدمج مع دعم روابط الماجنت وشجرة الملفات. معالجة البث عبر N_m3u8DL-RE وFFmpeg داخل حاويات Windows Job Objects المعزولة.
    </td>
  </tr>
</table>

<br>

## السمتان الفاتحة والداكنة

يدعم Radium كلاً من الوضع الفاتح والوضع الداكن بتكامل أصلي تام مع نظام Windows 11:

- **الوضع الفاتح (Light Mode):** خلفية بيضاء نقية (`#FFFFFF`)، ومساحات مرتفعة ناعمة (`#F5F7F9`)، ولمسات زرقاء سماوية (`#168BCB`)، ونصوص واضحة ومريحة للعين (`#202124`).
- **الوضع الداكن (Dark Mode):** درجات الرمادي الفحمي العميق (`#0F172A` / `#161618`)، وألواح كحلية، مع لمسة اللون الأحمر المرجاني الكلاسيكي (`#FF4757`).
- **زر تبديل مباشر:** زر خاص في شريط الأدوات الرئيسي يتيح التبديل الفوري بين المظهرين بنقرة واحدة مع حفظ الاختيار تلقائياً.
- **مزامنة النظام:** التكيف التلقائي مع خيار المظهر في إعدادات Windows (`AppsUseLightTheme`).

<br>

## الواجهة متعددة اللغات

تم تصميم Radium لجمهور عالمي، وهو مجهز بنظام لغات محلي مستقل يعمل دون أي حاجة للإنترنت.

> [!NOTE]
> ملف README في GitHub متوفر بـ **12 لغة رئيسية** (عبر شريط اللغات بالأعلى). في حين أن **تطبيق Radium لسطح المكتب يدعم حالياً 24 لغة مختلفة بأنظمتها الكتابية الأصلية**!

اللغات المتوفرة في تطبيق Radium:
الإنجليزية (`en`)، اليابانية (`ja`)، الصينية المبسطة (`zh-CN`)، الصينية التقليدية (`zh-TW`)، الكورية (`ko`)، الإسبانية (`es`)، الفرنسية (`fr`)، البرتغالية البرازيلية (`pt-BR`)، الألمانية (`de-DE`)، الروسية (`ru-RU`)، الإيطالية (`it-IT`)، العربية (`ar-SA` - مع دعم RTL)، بالإضافة إلى الفيتنامية، الهولندية، البولندية، التركية، الإندونيسية، الفارسية (RTL)، الأوكرانية، التشيكية، الرومانية، التايلاندية، السويدية واليونانية.

<br>

## لقطات الشاشة

<p align="center">
  <img src="docs/images/readme/radium-main-window.png" alt="الواجهة الرئيسية" width="100%">
  <br>
  <sub><b>النافذة الرئيسية:</b> لوحة متابعة فورية لمسارات التنزيل المقسم والسرعة الإجمالية.</sub>
</p>

<table>
  <tr>
    <td width="50%" valign="top">
      <img src="docs/images/readme/radium-new-download-dialog.jpg" alt="إضافة تنزيل" width="100%">
      <br>
      <sub><b>الفحص الأولي:</b> التحقق من دعم الخادم للتجزئة واحتساب المساحة.</sub>
    </td>
    <td width="50%" valign="top">
      <img src="docs/images/readme/radium-add-torrent-dialog.jpg" alt="إضافة تورنت" width="100%">
      <br>
      <sub><b>إدارة التورنت:</b> شجرة مجلدات تفاعلية لتحديد ملفات معينة وتأكيد السلامة.</sub>
    </td>
  </tr>
  <tr>
    <td colspan="2" valign="top">
      <img src="docs/images/readme/radium-settings-dialog.jpg" alt="مركز الإعدادات" width="100%">
      <br>
      <sub><b>مركز الإعدادات:</b> اختيار اللغة بالخط الأصلي، تفضيلات المظهر، وتحديد السرعة والبروكسي.</sub>
    </td>
  </tr>
</table>

<br>

## الترخيص والشكر

برنامج Radium Download Manager مرخص بموجب **رخصة أباتشي 2.0** ([Apache-2.0](LICENSE)).

المكونات الخارجية: [N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) (MIT)، [FFmpeg](https://ffmpeg.org/) (LGPL-2.1+ / LGPL-3.0)، [Microsoft WIL](https://github.com/microsoft/wil) (MIT)، [SQLite](https://www.sqlite.org/) (الملكية العامة)، [nlohmann/json](https://github.com/nlohmann/json) (MIT).

<br>

<p align="center">
  <sub>Radium Download Manager · مدير تنزيلات أصلي عالي الأداء ومفتوح المصدر لنظام Windows.</sub><br>
  <sub>مبني باستخدام C++20 وWin32. مرخص تحت Apache-2.0.</sub>
</p>

</div>
