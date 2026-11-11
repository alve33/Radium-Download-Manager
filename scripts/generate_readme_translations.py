#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generate complete, professional translations for Radium's 11 important README languages.
"""

import os
import sys

# Language configs: (filename, native_name, lang_code, selector_highlight_index)
# Languages in exact selector order:
# 0: English (README.md)
# 1: 日本語 (README.ja.md)
# 2: 简体中文 (README.zh-CN.md)
# 3: 繁體中文 (README.zh-TW.md)
# 4: 한국어 (README.ko.md)
# 5: Español (README.es.md)
# 6: Français (README.fr.md)
# 7: Português (README.pt-BR.md)
# 8: Deutsch (README.de-DE.md)
# 9: Русский (README.ru-RU.md)
# 10: Italiano (README.it-IT.md)
# 11: العربية (README.ar-SA.md)

LANG_ITEMS = [
    ("English", "README.md"),
    ("日本語", "README.ja.md"),
    ("简体中文", "README.zh-CN.md"),
    ("繁體中文", "README.zh-TW.md"),
    ("한국어", "README.ko.md"),
    ("Español", "README.es.md"),
    ("Français", "README.fr.md"),
    ("Português", "README.pt-BR.md"),
    ("Deutsch", "README.de-DE.md"),
    ("Русский", "README.ru-RU.md"),
    ("Italiano", "README.it-IT.md"),
    ("العربية", "README.ar-SA.md"),
]

def make_selector(current_index):
    parts = []
    for idx, (name, link) in enumerate(LANG_ITEMS):
        if idx == current_index:
            parts.append(f"<b>{name}</b>")
        else:
            parts.append(f'<a href="{link}">{name}</a>')
    return '<p align="center">\n  ' + " · ".join(parts) + "\n</p>"

print("Selector generator ready.")
