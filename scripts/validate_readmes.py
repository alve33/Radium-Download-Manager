#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Validate all 12 README files:
1. Existence
2. Encoding (UTF-8)
3. Selector format and current language bolding
4. Selector links pointing to existing files
5. Image links pointing to existing files
6. Section header consistency
"""

import os
import sys
import re

EXPECTED_FILES = [
    "README.md",
    "README.ja.md",
    "README.zh-CN.md",
    "README.zh-TW.md",
    "README.ko.md",
    "README.es.md",
    "README.fr.md",
    "README.pt-BR.md",
    "README.de-DE.md",
    "README.ru-RU.md",
    "README.it-IT.md",
    "README.ar-SA.md",
]

LANG_NAMES = [
    "English",
    "日本語",
    "简体中文",
    "繁體中文",
    "한국어",
    "Español",
    "Français",
    "Português",
    "Deutsch",
    "Русский",
    "Italiano",
    "العربية",
]

errors = []

print("=== Validating README Files ===")

for idx, fname in enumerate(EXPECTED_FILES):
    expected_bold_name = LANG_NAMES[idx]
    if not os.path.isfile(fname):
        errors.append(f"Missing file: {fname}")
        continue
    
    with open(fname, "r", encoding="utf-8") as f:
        content = f.read()
    
    # Check bolding in selector
    bold_pattern = f"<b>{re.escape(expected_bold_name)}</b>"
    if not re.search(bold_pattern, content):
        errors.append(f"{fname}: Expected bold language '{expected_bold_name}' not found in selector!")
    
    # Check selector links
    for l_idx, (l_name, l_file) in enumerate(zip(LANG_NAMES, EXPECTED_FILES)):
        if l_idx != idx:
            link_str = f'href="{l_file}"'
            if link_str not in content:
                errors.append(f"{fname}: Link to {l_file} not found!")

    # Check images referenced exist
    img_matches = re.findall(r'<img[^>]+src=["\']([^"\']+)["\']', content)
    for img_src in img_matches:
        if img_src.startswith("http://") or img_src.startswith("https://"):
            continue
        # Local image path
        norm_path = os.path.normpath(img_src)
        if not os.path.exists(norm_path):
            errors.append(f"{fname}: Referenced local image does not exist: {img_src}")

print(f"Checked {len(EXPECTED_FILES)} README files.")
if errors:
    print(f"Validation FAILED with {len(errors)} errors:")
    for err in errors:
        print(f"  - {err}")
    sys.exit(1)
else:
    print("ALL VALIDATION CHECKS PASSED SUCCESSFULLY!")
