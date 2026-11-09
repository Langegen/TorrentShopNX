#include "homebrew_view_model.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace homebrew {

namespace {

std::string lower(std::string s) {
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x80) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const Installed* lookup(const InstalledMap& m, const std::string& name) {
    auto it = m.find(lower(name));
    return it == m.end() ? nullptr : &it->second;
}

bool matches(const Package& p, const std::string& q) {
    if (q.empty()) return true;
    auto has = [&](const std::string& s) { return lower(s).find(q) != std::string::npos; };
    if (has(p.title) || has(p.name) || has(p.author) || has(p.description)) return true;
    for (const auto& d : p.description_i18n)
        if (has(d.second)) return true;
    return false;
}

}  // namespace

bool inCategory(const Package& pkg, Category c, Status status) {
    const std::string cat = lower(pkg.category);
    switch (c) {
        case Category::All: return true;
        case Category::Games: return cat == "game";
        case Category::Emulators: return cat == "emu";
        case Category::Themes: return cat == "theme";
        case Category::Overlays: return (pkg.kinds & KindOverlay) != 0;
        case Category::Sysmodules: return (pkg.kinds & KindSysmodule) != 0;
        case Category::Installed: return status != Status::NotInstalled;
        case Category::Apps:
            // Tools: not a game, emulator or theme, and something to launch (or kinds unknown).
            return cat != "game" && cat != "emu" && cat != "theme" && ((pkg.kinds & KindApp) != 0 || pkg.kinds == 0);
        case Category::Count: break;
    }
    return false;
}

std::vector<ViewItem> buildView(const PackageList& pkgs, const InstalledMap& installed, Category c, SortMode sort,
                                const std::string& query, const std::string& today) {
    std::string q = lower(query);
    while (!q.empty() && std::isspace(static_cast<unsigned char>(q.back()))) q.pop_back();
    while (!q.empty() && std::isspace(static_cast<unsigned char>(q.front()))) q.erase(q.begin());

    std::vector<ViewItem> out;
    for (const Package& p : pkgs) {
        const Installed* in = lookup(installed, p.name);
        const Status st = in ? statusFor(p, *in) : Status::NotInstalled;
        if (!inCategory(p, c, st) || !matches(p, q)) continue;
        ViewItem it;
        it.pkg = &p;
        it.status = st;
        it.untracked = in && in->untracked;
        it.is_new = isNew(p, today);
        out.push_back(it);
    }

    auto by_name = [](const ViewItem& a, const ViewItem& b) { return lower(a.pkg->title) < lower(b.pkg->title); };
    auto by_updated = [&](const ViewItem& a, const ViewItem& b) {
        if (a.pkg->updated != b.pkg->updated) return a.pkg->updated > b.pkg->updated;
        return by_name(a, b);
    };
    std::stable_sort(out.begin(), out.end(), [&](const ViewItem& a, const ViewItem& b) {
        if (c == Category::Installed) {
            const bool ua = a.status == Status::UpdateAvailable, ub = b.status == Status::UpdateAvailable;
            if (ua != ub) return ua;
            return by_name(a, b);
        }
        switch (sort) {
            case SortMode::Popular:
                if (a.pkg->downloads != b.pkg->downloads) return a.pkg->downloads > b.pkg->downloads;
                return by_updated(a, b);
            case SortMode::Updated: return by_updated(a, b);
            case SortMode::Name:
            case SortMode::Count: break;
        }
        return by_name(a, b);
    });
    return out;
}

std::array<int, kCategoryCount> countByCategory(const PackageList& pkgs, const InstalledMap& installed) {
    std::array<int, kCategoryCount> n{};
    for (const Package& p : pkgs) {
        const Installed* in = lookup(installed, p.name);
        const Status st = in ? statusFor(p, *in) : Status::NotInstalled;
        for (size_t i = 0; i < kCategoryCount; ++i)
            if (inCategory(p, static_cast<Category>(i), st)) ++n[i];
    }
    return n;
}

std::string displayDescription(const Package& pkg, const std::string& locale) {
    auto it = pkg.description_i18n.find(locale);
    if (it == pkg.description_i18n.end()) {
        const std::string lang = locale.substr(0, locale.find('-'));
        it = pkg.description_i18n.find(lang);
    }
    if (it != pkg.description_i18n.end() && !it->second.empty()) return it->second;
    return pkg.description;
}

std::string formatSize(int64_t bytes, const std::array<std::string, 4>& units) {
    if (bytes <= 0) return "";
    double v = static_cast<double>(bytes);
    size_t u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        ++u;
    }
    char buf[48];
    if (u == 0 || v >= 100.0) std::snprintf(buf, sizeof(buf), "%.0f", v);
    else std::snprintf(buf, sizeof(buf), "%.1f", v);
    return std::string(buf) + " " + units[u];
}

std::string plainText(const std::string& md) {
    std::string out;
    size_t pos = 0;
    while (pos <= md.size()) {
        size_t end = md.find('\n', pos);
        if (end == std::string::npos) end = md.size();
        std::string line = md.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Leading spaces kept as they are; the mark after them is what counts.
        size_t lead = 0;
        while (lead < line.size() && line[lead] == ' ') ++lead;
        std::string body = line.substr(lead);
        if (!body.empty() && body[0] == '#') {
            size_t h = 0;
            while (h < body.size() && body[h] == '#') ++h;
            body = body.substr(h);
            while (!body.empty() && body[0] == ' ') body.erase(0, 1);
        } else if (body.size() > 1 && (body[0] == '-' || body[0] == '*' || body[0] == '+') && body[1] == ' ') {
            body = "• " + body.substr(2);
        }

        std::string clean;
        for (size_t i = 0; i < body.size(); ++i) {
            const char c = body[i];
            if ((c == '*' || c == '_') && i + 1 < body.size() && body[i + 1] == c) {  // **bold**, __bold__
                ++i;
                continue;
            }
            if (c == '`') continue;
            if (c == '[') {  // [text](url) -> text
                const size_t close = body.find("](", i);
                const size_t paren = close == std::string::npos ? std::string::npos : body.find(')', close);
                if (close != std::string::npos && paren != std::string::npos) {
                    clean += body.substr(i + 1, close - i - 1);
                    i = paren;
                    continue;
                }
            }
            clean += c;
        }
        out += std::string(lead, ' ') + clean;
        if (end == md.size()) break;
        out += '\n';
        pos = end + 1;
    }
    // At most one empty line in a row.
    std::string squeezed;
    int newlines = 0;
    for (char c : out) {
        newlines = c == '\n' ? newlines + 1 : 0;
        if (newlines <= 2) squeezed += c;
    }
    while (!squeezed.empty() && (squeezed.back() == '\n' || squeezed.back() == ' ')) squeezed.pop_back();
    return squeezed;
}

bool isKnownLicense(const std::string& license) {
    const std::string l = lower(license);
    return !l.empty() && l != "n/a" && l != "na" && l != "none" && l != "unknown" && l != "noassertion";
}

std::string displayDate(const std::string& iso) {
    if (iso.size() >= 10 && iso[4] == '-' && iso[7] == '-')
        return iso.substr(8, 2) + "." + iso.substr(5, 2) + "." + iso.substr(0, 4);
    return iso;
}

}  // namespace homebrew
