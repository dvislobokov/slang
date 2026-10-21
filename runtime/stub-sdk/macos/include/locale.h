/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_LOCALE_H
#define SLANG_STUB_LOCALE_H
#define LC_ALL 0
#define LC_COLLATE 1
#define LC_CTYPE 2
#define LC_MONETARY 3
#define LC_NUMERIC 4
#define LC_TIME 5
#define LC_MESSAGES 6
char *setlocale(int, const char *);
#endif
