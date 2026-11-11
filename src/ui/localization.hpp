// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace openidm::ui {

struct LanguageOption {
    std::string locale;
    std::string native_name;
};

class LocalizationManager {
public:
    explicit LocalizationManager(std::filesystem::path catalog_root = {});

    bool SetLocale(std::string_view locale);
    std::string GetLocale() const;
    std::string Translate(std::string_view english) const;
    bool IsRightToLeft() const;
    static const std::vector<LanguageOption>& Languages();
    static std::string ResolveSystemLocale();
    static std::string NormalizeLocale(std::string_view locale);

private:
    bool LoadCatalog(std::string_view locale, std::map<std::string, std::string>& out) const;

    std::filesystem::path m_catalog_root;
    mutable std::mutex m_mutex;
    std::string m_locale{"en"};
    std::map<std::string, std::string> m_messages;
};

LocalizationManager& GetLocalizationManager();

} // namespace openidm::ui
