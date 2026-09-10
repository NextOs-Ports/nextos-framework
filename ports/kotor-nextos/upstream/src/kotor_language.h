/* SPDX-License-Identifier: MIT */
#ifndef KOTOR_LANGUAGE_H
#define KOTOR_LANGUAGE_H

/*
 * Translate the framework's lowercase language code to the exact integer
 * returned by com.aspyr.kotor.KOTOR.getCurrentLanguage().
 */
int kotor_language_id_from_code(const char *code);

#endif
