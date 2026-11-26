#include "../source/catalog/localized_genre_aliases.hpp"
#include <algorithm>
#include <cassert>

int main() {
    const auto& aliases = catalog::localizedGenreAliases();
    auto has = [&](const char* label, const char* id) {
        const auto it = aliases.find(label);
        return it != aliases.end() && std::find(it->second.begin(), it->second.end(), id) != it->second.end();
    };
    assert(has("3d平台跳跃", "platformer"));
    assert(has("roguelite 轻度肉鸽", "roguelike"));
    assert(has("集换式卡牌", "card_board"));
    assert(has("清版动作", "action"));
    assert(has("潜行", "stealth") && has("潜行", "action"));
    assert(has("动作角色扮演 (arpg)", "rpg") && has("动作角色扮演 (arpg)", "action"));
    assert(has("动作冒险", "action") && has("动作冒险", "adventure"));
    assert(has("战术策略", "strategy"));
}
