/* The port's own in-game texts in the game's language (BB_LANGUAGE, the PS4 language id the
 * launcher passes): the menus, cards, notices and options the port adds. The table is
 * src/runtime_texts.inc, generated from scripts/octr_texts.py (the same one the menus the port
 * builds at start use). A text with no entry, or English/Arabic, stays as written.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"

#include <stdlib.h>
#include <string.h>

enum { TEXT_MX, TEXT_ES, TEXT_FR, TEXT_PT, TEXT_DE, TEXT_IT, TEXT_COLUMNS };

static const char *const text_table[][1 + TEXT_COLUMNS] = {
#include "runtime_texts.inc"
};

/* PS4 language id -> column; -1 English (and Arabic, which has no column). */
static int text_column_of(int id) {
    switch (id) {
    case 20: return TEXT_MX;
    case 3: return TEXT_ES;
    case 2: case 22: return TEXT_FR;
    case 17: case 7: return TEXT_PT;
    case 4: return TEXT_DE;
    case 5: return TEXT_IT;
    default: return -1;
    }
}

static int text_column = -2;

int runtime_text_column(void) {
    if (text_column == -2) {
        const char *v = getenv("BB_LANGUAGE");
        text_column = text_column_of(v ? atoi(v) : 1);
    }
    return text_column;
}

void runtime_text_set_language(int id) { text_column = text_column_of(id); }

const char *runtime_text(const char *english) {
    const int c = runtime_text_column();
    if (!english || c < 0) return english;
    for (size_t i = 0; i < sizeof text_table / sizeof *text_table; ++i)
        if (!strcmp(text_table[i][0], english)) return text_table[i][1 + c];
    return english;
}

int runtime_text_is(const char *shown, const char *english) {
    return shown && english && (!strcmp(shown, english) || !strcmp(shown, runtime_text(english)));
}
