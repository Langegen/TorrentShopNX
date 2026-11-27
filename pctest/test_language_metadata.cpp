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
    assert(chineseLanguageLabel(LangChs, true, true) == "簡中");
    assert(chineseLanguageLabel(LangCht, true) == "繁中");
    assert(chineseLanguageLabel(LangChs | LangCht, false) == "CHI");

    // Real catalog strings from RU_catalog.json / ZH_Hans_catalog.json
    assert(metadataLanguageMask("русский, английский [rus / eng / multi 17]") == (LangRus | LangEng | LangMulti));
    assert(metadataLanguageMask("английский [eng / multi 8]") == (LangEng | LangMulti));
    assert(metadataIsMulti(metadataLanguageMask("английский [eng / multi 8]")));
    assert(metadataLanguageMask("[multi 9]") == LangMulti);
    assert(metadataIsMulti(metadataLanguageMask("[multi 9]")));
    assert(metadataLanguageMask(
        "русский (rus); английский (eng); немецкий (deu); французский (fra); "
        "испанский (spa); итальянский (ita); португальский бразильский (por-br)") ==
        (LangRus | LangEng | LangGer | LangFra | LangSpa | LangIta | LangPor));
    assert(metadataLanguageMask("русский (официальный перевод) [rus]") == LangRus);
}
