# SPDX-License-Identifier: GPL-2.0-or-later
"""Launcher translations: the English text is the key, and locales/<code>.json maps it to another
language. A text missing from a file shows in English. The same languages as the Windows launcher.

ui_language in the launcher settings: one of the codes below, or "" for the system's language.
"""
import json
import os
from pathlib import Path

LOCALES_DIR = Path(__file__).resolve().parent / "locales"
# (label, code) for the Launcher language choice: each language named in itself.
UI_LANGUAGES = [("System", ""), ("English", "en"), ("Español", "es"), ("Português (Brasil)", "pt_BR"),
                ("Français", "fr"), ("Deutsch", "de"), ("Italiano", "it"), ("日本語", "ja"),
                ("简体中文", "zh_CN"), ("Русский", "ru")]
CODES = {code for _label, code in UI_LANGUAGES if code}
# The labels above stay in their own language (not translated).
NATIVE_LABELS = {label for label, code in UI_LANGUAGES if code}

_language = "en"
_table = {}


def system_language():
    """The desktop's language among CODES (LC_ALL, LC_MESSAGES, LANG, LANGUAGE), else English."""
    for key in ("LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"):
        value = os.environ.get(key, "").split(":")[0]
        if not value or value in ("C", "POSIX", "C.UTF-8"):
            continue
        locale = value.split(".")[0].replace("-", "_")
        if locale in CODES:
            return locale
        short = locale.split("_")[0].lower()
        if short == "pt":
            return "pt_BR"
        if short == "zh":
            return "zh_CN"
        if short in CODES:
            return short
        return "en"
    return "en"


def load_table(code):
    """locales/<code>.json as a dict; {} for English or a missing or broken file."""
    if code == "en":
        return {}
    try:
        table = json.loads((LOCALES_DIR / f"{code}.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return {k: v for k, v in table.items() if isinstance(v, str) and v}


def set_language(choice):
    """choice: a code of UI_LANGUAGES, or "" (the system's language)."""
    global _language, _table
    _language = choice if choice in CODES else system_language()
    _table = load_table(_language)


def language():
    return _language


def menu_language():
    """The in-game menu's language (BB_MENU_LANGUAGE): it has English and Russian only."""
    return "ru" if _language == "ru" else "en"


def tr(text):
    if text in NATIVE_LABELS:
        return text
    return _table.get(text, text)
