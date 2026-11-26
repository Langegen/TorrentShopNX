#include "../source/utils/language_metadata.h"
#include <cassert>

int main() {
    using namespace util;
    assert(metadataLanguageMask("简体中文") == LangChs);
    assert(metadataLanguageMask("繁體中文") == LangCht);
    assert(metadataLanguageMask("中文") == LangChi);
    assert(metadataLanguageMask("简体中文、english；日本語") == (LangChs | LangEng | LangJpn));
    assert(metadataLanguageMask("[rus / eng]") == (LangRus | LangEng));
    assert(metadataLanguageMask("русский, английский") == (LangRus | LangEng));
    assert(metadataLanguageMask("chinese (traditional)") == LangCht);
    assert(metadataLanguageMask("no chinese") == 0);
    assert(metadataLanguageMask("unknown") == 0);
    assert(titleLanguageTags("chinese adventure [NSP]") == 0);
    assert(titleLanguageTags("sample [nsp] [chs / eng]") == (LangChs | LangEng));
    assert(!metadataIsMulti(LangChi | LangChs));
    assert(metadataIsMulti(LangChs | LangEng));
    assert(metadataIsMulti(LangMulti));
    assert(chineseLanguageLabel(LangChs, true) == "简中");
    assert(chineseLanguageLabel(LangCht, true) == "繁中");
    assert(chineseLanguageLabel(LangChs | LangCht, false) == "CHI");
}
