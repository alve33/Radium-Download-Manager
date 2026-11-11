// SPDX-License-Identifier: Apache-2.0
#include "ui/localization.hpp"

#include "common/json_mini.hpp"
#include "common/path_sanitizer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace openidm::ui {
namespace {

std::filesystem::path DefaultCatalogRoot() {
    return openidm::PathSanitizer::GetExecutableDir() / "translations";
}

const std::vector<LanguageOption> kLanguages{
    {"en", "English"}, {"ja", "日本語"}, {"zh-CN", "简体中文"}, {"zh-TW", "繁體中文"},
    {"ko", "한국어"}, {"es", "Español"}, {"fr", "Français"}, {"vi", "Tiếng Việt"},
    {"pt-BR", "Português (Brasil)"}, {"de-DE", "Deutsch"}, {"ru-RU", "Русский"},
    {"it-IT", "Italiano"}, {"nl-NL", "Nederlands"}, {"pl-PL", "Polski"},
    {"tr-TR", "Türkçe"}, {"id-ID", "Bahasa Indonesia"}, {"ar-SA", "العربية"},
    {"fa-IR", "فارسی"}, {"uk-UA", "Українська"}, {"cs-CZ", "Čeština"},
    {"ro-RO", "Română"}, {"th-TH", "ไทย"}, {"sv-SE", "Svenska"}, {"el-GR", "Ελληνικά"}
};

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

LocalizationManager::LocalizationManager(std::filesystem::path catalog_root)
    : m_catalog_root(catalog_root.empty() ? DefaultCatalogRoot() : std::move(catalog_root)) {
    SetLocale("en");
}

const std::vector<LanguageOption>& LocalizationManager::Languages() {
    return kLanguages;
}

std::string LocalizationManager::NormalizeLocale(std::string_view requested) {
    if (requested == "system") return ResolveSystemLocale();
    const std::string normalized = Lower(std::string(requested));
    for (const auto& language : kLanguages) {
        if (Lower(language.locale) == normalized) return language.locale;
    }
    const auto dash = normalized.find('-');
    const std::string base = normalized.substr(0, dash);
    if (base == "zh") return normalized.find("tw") != std::string::npos || normalized.find("hk") != std::string::npos ? "zh-TW" : "zh-CN";
    if (base == "pt") return "pt-BR";
    for (const auto& language : kLanguages) {
        if (Lower(language.locale).substr(0, Lower(language.locale).find('-')) == base) return language.locale;
    }
    return {};
}

std::string LocalizationManager::ResolveSystemLocale() {
#ifdef _WIN32
    wchar_t locale_name[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale_name, LOCALE_NAME_MAX_LENGTH) > 0) {
        int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, locale_name, -1, nullptr, 0, nullptr, nullptr);
        if (size > 1) {
            std::string locale(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, locale_name, -1, locale.data(), size, nullptr, nullptr);
            locale.resize(static_cast<size_t>(size - 1));
            const std::string resolved = NormalizeLocale(locale);
            return resolved.empty() ? "en" : resolved;
        }
    }
#else
    if (const char* env_locale = std::getenv("LC_ALL"); env_locale && *env_locale) {
        const std::string resolved = NormalizeLocale(env_locale);
        if (!resolved.empty()) return resolved;
    }
    if (const char* env_locale = std::getenv("LANG"); env_locale && *env_locale) {
        const std::string resolved = NormalizeLocale(env_locale);
        if (!resolved.empty()) return resolved;
    }
#endif
    return "en";
}

bool LocalizationManager::LoadCatalog(std::string_view locale, std::map<std::string, std::string>& out) const {
    std::ifstream file(m_catalog_root / "desktop.json", std::ios::binary);
    if (!file) return false;
    const std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    try {
        const auto root = openidm::json::Parse(contents);
        const auto& messages = root["locales"][std::string(locale)];
        if (!messages.is_object()) return false;
        for (const auto& [key, value] : messages.as_object()) {
            if (value.is_string()) out.emplace(key, value.as_string());
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool LocalizationManager::SetLocale(std::string_view requested) {
    std::string normalized = NormalizeLocale(requested);
    const bool supported = requested == "system" || !normalized.empty();
    if (normalized.empty()) normalized = "en";
    std::map<std::string, std::string> english;
    LoadCatalog("en", english);
    std::map<std::string, std::string> selected;
    const bool loaded = normalized == "en" ? true : LoadCatalog(normalized, selected);
    if (!loaded && normalized != "en") normalized = "en";
    for (const auto& [key, value] : english) selected.try_emplace(key, value);
    std::lock_guard lock(m_mutex);
    m_locale = std::move(normalized);
    m_messages = std::move(selected);
    return supported && (loaded || m_locale == "en");
}

std::string LocalizationManager::GetLocale() const {
    std::lock_guard lock(m_mutex);
    return m_locale;
}

std::string LocalizationManager::Translate(std::string_view english) const {
    std::lock_guard lock(m_mutex);
    const auto found = m_messages.find(std::string(english));
    return found == m_messages.end() ? std::string(english) : found->second;
}

bool LocalizationManager::IsRightToLeft() const {
    std::lock_guard lock(m_mutex);
    return m_locale == "ar-SA" || m_locale == "fa-IR";
}

LocalizationManager& GetLocalizationManager() {
    static LocalizationManager manager;
    return manager;
}

} // namespace openidm::ui
