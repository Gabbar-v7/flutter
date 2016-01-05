/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// #define VERBOSE_DEBUG

#define LOG_TAG "Minikin"
#include <cutils/log.h>

#include "unicode/unistr.h"
#include "unicode/unorm2.h"

#include "FontLanguageListCache.h"
#include "MinikinInternal.h"
#include <minikin/FontCollection.h>

using std::vector;

namespace android {

template <typename T>
static inline T max(T a, T b) {
    return a>b ? a : b;
}

uint32_t FontCollection::sNextId = 0;

FontCollection::FontCollection(const vector<FontFamily*>& typefaces) :
    mMaxChar(0) {
    AutoMutex _l(gMinikinLock);
    mId = sNextId++;
    vector<uint32_t> lastChar;
    size_t nTypefaces = typefaces.size();
#ifdef VERBOSE_DEBUG
    ALOGD("nTypefaces = %zd\n", nTypefaces);
#endif
    const FontStyle defaultStyle;
    for (size_t i = 0; i < nTypefaces; i++) {
        FontFamily* family = typefaces[i];
        MinikinFont* typeface = family->getClosestMatch(defaultStyle).font;
        if (typeface == NULL) {
            continue;
        }
        family->RefLocked();
        const SparseBitSet* coverage = family->getCoverage();
        if (coverage == nullptr) {
            family->UnrefLocked();
            continue;
        }
        mFamilies.push_back(family);  // emplace_back would be better
        mMaxChar = max(mMaxChar, coverage->length());
        lastChar.push_back(coverage->nextSetBit(0));
    }
    nTypefaces = mFamilies.size();
    LOG_ALWAYS_FATAL_IF(nTypefaces == 0,
        "Font collection must have at least one valid typeface");
    size_t nPages = (mMaxChar + kPageMask) >> kLogCharsPerPage;
    size_t offset = 0;
    // TODO: Use variation selector map for mRanges construction.
    // A font can have a glyph for a base code point and variation selector pair but no glyph for
    // the base code point without variation selector. The family won't be listed in the range in
    // this case.
    for (size_t i = 0; i < nPages; i++) {
        Range dummy;
        mRanges.push_back(dummy);
        Range* range = &mRanges.back();
#ifdef VERBOSE_DEBUG
        ALOGD("i=%zd: range start = %zd\n", i, offset);
#endif
        range->start = offset;
        for (size_t j = 0; j < nTypefaces; j++) {
            if (lastChar[j] < (i + 1) << kLogCharsPerPage) {
                FontFamily* family = mFamilies[j];
                mFamilyVec.push_back(family);
                offset++;
                uint32_t nextChar = family->getCoverage()->nextSetBit((i + 1) << kLogCharsPerPage);
#ifdef VERBOSE_DEBUG
                ALOGD("nextChar = %d (j = %zd)\n", nextChar, j);
#endif
                lastChar[j] = nextChar;
            }
        }
        range->end = offset;
    }
}

FontCollection::~FontCollection() {
    for (size_t i = 0; i < mFamilies.size(); i++) {
        mFamilies[i]->UnrefLocked();
    }
}

// Implement heuristic for choosing best-match font. Here are the rules:
// 1. If first font in the collection has the character, it wins.
// 2. If a font matches language, it gets a score of 2.
// 3. Matching the "compact" or "elegant" variant adds one to the score.
// 4. If there is a variation selector and a font supports the complete variation sequence, we add
//    8 to the score.
// 5. If there is a color variation selector (U+FE0F), we add 4 to the score if the font is an emoji
//    font. This additional score of 4 is only given if the base character is supported in the font,
//    but not the whole variation sequence.
// 6. If there is a text variation selector (U+FE0E), we add 4 to the score if the font is not an
//    emoji font. This additional score of 4 is only given if the base character is supported in the
//    font, but not the whole variation sequence.
// 7. Highest score wins, with ties resolved to the first font.
FontFamily* FontCollection::getFamilyForChar(uint32_t ch, uint32_t vs,
            uint32_t langListId, int variant) const {
    if (ch >= mMaxChar) {
        return NULL;
    }

    const FontLanguages& langList = FontLanguageListCache::getById(langListId);
    // TODO: use all languages in langList.
    const FontLanguage lang = (langList.size() == 0) ? FontLanguage() : langList[0];

    // Even if the font supports variation sequence, mRanges isn't aware of the base character of
    // the sequence. Search all FontFamilies if variation sequence is specified.
    // TODO: Always use mRanges for font search.
    const std::vector<FontFamily*>& familyVec = (vs == 0) ? mFamilyVec : mFamilies;
    Range range;
    if (vs == 0) {
        range = mRanges[ch >> kLogCharsPerPage];
    } else {
        range = { 0, mFamilies.size() };
    }

#ifdef VERBOSE_DEBUG
    ALOGD("querying range %zd:%zd\n", range.start, range.end);
#endif
    FontFamily* bestFamily = nullptr;
    int bestScore = -1;
    for (size_t i = range.start; i < range.end; i++) {
        FontFamily* family = familyVec[i];
        const bool hasVSGlyph = (vs != 0) && family->hasVariationSelector(ch, vs);
        if (hasVSGlyph || family->getCoverage()->get(ch)) {
            if ((vs == 0 || hasVSGlyph) && mFamilies[0] == family) {
                // If the first font family in collection supports the given character or sequence,
                // always use it.
                return family;
            }
            int score = lang.match(family->lang()) * 2;
            if (family->variant() == 0 || family->variant() == variant) {
                score++;
            }
            if (hasVSGlyph) {
                score += 8;
            } else if (((vs == 0xFE0F) && family->lang().hasEmojiFlag()) ||
                    ((vs == 0xFE0E) && !family->lang().hasEmojiFlag())) {
                score += 4;
            }
            if (score > bestScore) {
                bestScore = score;
                bestFamily = family;
            }
        }
    }
    if (bestFamily == nullptr && vs != 0) {
        // If no fonts support the codepoint and variation selector pair,
        // fallback to select a font family that supports just the base
        // character, ignoring the variation selector.
        return getFamilyForChar(ch, 0, langListId, variant);
    }
    if (bestFamily == nullptr && !mFamilyVec.empty()) {
        UErrorCode errorCode = U_ZERO_ERROR;
        const UNormalizer2* normalizer = unorm2_getNFDInstance(&errorCode);
        if (U_SUCCESS(errorCode)) {
            UChar decomposed[4];
            int len = unorm2_getRawDecomposition(normalizer, ch, decomposed, 4, &errorCode);
            if (U_SUCCESS(errorCode) && len > 0) {
                int off = 0;
                U16_NEXT_UNSAFE(decomposed, off, ch);
                return getFamilyForChar(ch, vs, langListId, variant);
            }
        }
        bestFamily = mFamilies[0];
    }
    return bestFamily;
}

const uint32_t NBSP = 0xa0;
const uint32_t ZWJ = 0x200c;
const uint32_t ZWNJ = 0x200d;
const uint32_t KEYCAP = 0x20e3;
const uint32_t HYPHEN = 0x2010;
const uint32_t NB_HYPHEN = 0x2011;

// Characters where we want to continue using existing font run instead of
// recomputing the best match in the fallback list.
static const uint32_t stickyWhitelist[] = { '!', ',', '-', '.', ':', ';', '?', NBSP, ZWJ, ZWNJ,
        KEYCAP, HYPHEN, NB_HYPHEN };

static bool isStickyWhitelisted(uint32_t c) {
    for (size_t i = 0; i < sizeof(stickyWhitelist) / sizeof(stickyWhitelist[0]); i++) {
        if (stickyWhitelist[i] == c) return true;
    }
    return false;
}

static bool isVariationSelector(uint32_t c) {
    return (0xFE00 <= c && c <= 0xFE0F) || (0xE0100 <= c && c <= 0xE01EF);
}

bool FontCollection::hasVariationSelector(uint32_t baseCodepoint,
        uint32_t variationSelector) const {
    if (!isVariationSelector(variationSelector)) {
        return false;
    }
    if (baseCodepoint >= mMaxChar) {
        return false;
    }
    // Currently mRanges can not be used here since it isn't aware of the variation sequence.
    // TODO: Use mRanges for narrowing down the search range.
    for (size_t i = 0; i < mFamilies.size(); i++) {
        AutoMutex _l(gMinikinLock);
        if (mFamilies[i]->hasVariationSelector(baseCodepoint, variationSelector)) {
          return true;
        }
    }
    return false;
}

void FontCollection::itemize(const uint16_t *string, size_t string_size, FontStyle style,
        vector<Run>* result) const {
    const uint32_t langListId = style.getLanguageListId();
    int variant = style.getVariant();
    FontFamily* lastFamily = NULL;
    Run* run = NULL;

    if (string_size == 0) {
        return;
    }

    const uint32_t kEndOfString = 0xFFFFFFFF;

    uint32_t nextCh = 0;
    uint32_t prevCh = 0;
    size_t nextUtf16Pos = 0;
    size_t readLength = 0;
    U16_NEXT(string, readLength, string_size, nextCh);

    do {
        const uint32_t ch = nextCh;
        const size_t utf16Pos = nextUtf16Pos;
        nextUtf16Pos = readLength;
        if (readLength < string_size) {
            U16_NEXT(string, readLength, string_size, nextCh);
        } else {
            nextCh = kEndOfString;
        }

        bool shouldContinueRun = false;
        if (lastFamily != nullptr) {
            if (isStickyWhitelisted(ch)) {
                // Continue using existing font as long as it has coverage and is whitelisted
                shouldContinueRun = lastFamily->getCoverage()->get(ch);
            } else if (isVariationSelector(ch)) {
                // Always continue if the character is a variation selector.
                shouldContinueRun = true;
            }
        }

        if (!shouldContinueRun) {
            FontFamily* family = getFamilyForChar(ch, isVariationSelector(nextCh) ? nextCh : 0,
                    langListId, variant);
            if (utf16Pos == 0 || family != lastFamily) {
                size_t start = utf16Pos;
                // Workaround for Emoji keycap until we implement per-cluster font
                // selection: if keycap is found in a different font that also
                // supports previous char, attach previous char to the new run.
                // Bug 7557244.
                if (ch == KEYCAP && utf16Pos != 0 && family && family->getCoverage()->get(prevCh)) {
                    const size_t prevChLength = U16_LENGTH(prevCh);
                    run->end -= prevChLength;
                    if (run->start == run->end) {
                        result->pop_back();
                    }
                    start -= prevChLength;
                }
                Run dummy;
                result->push_back(dummy);
                run = &result->back();
                if (family == NULL) {
                    run->fakedFont.font = NULL;
                } else {
                    run->fakedFont = family->getClosestMatch(style);
                }
                lastFamily = family;
                run->start = start;
            }
        }
        prevCh = ch;
        run->end = nextUtf16Pos;  // exclusive
    } while (nextCh != kEndOfString);
}

MinikinFont* FontCollection::baseFont(FontStyle style) {
    return baseFontFaked(style).font;
}

FakedFont FontCollection::baseFontFaked(FontStyle style) {
    if (mFamilies.empty()) {
        return FakedFont();
    }
    return mFamilies[0]->getClosestMatch(style);
}

uint32_t FontCollection::getId() const {
    return mId;
}

void FontCollection::purgeFontFamilyHbFontCache() const {
    assertMinikinLocked();
    for (size_t i = 0; i < mFamilies.size(); ++i) {
        mFamilies[i]->purgeHbFontCache();
    }
}

}  // namespace android
