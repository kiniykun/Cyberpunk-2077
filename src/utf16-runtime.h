#pragma once
#include <cstddef>
#include <cstdarg>
#include <cwchar>
#include <cstdio>

namespace cpweb {
int vsnprintf_ansi(char*, size_t, const char*, va_list);
static_assert(sizeof(wchar_t) == 2, "The game UTF-16 ABI requires -fshort-wchar");
size_t wcslen(const wchar_t*);
size_t wcsnlen(const wchar_t*, size_t);
wchar_t* wcsncpy(wchar_t*, const wchar_t*, size_t);
wchar_t* wcsncat(wchar_t*, const wchar_t*, size_t);
int wcscmp(const wchar_t*, const wchar_t*);
int wcsncmp(const wchar_t*, const wchar_t*, size_t);
const wchar_t* wcschr(const wchar_t*, wchar_t);
wchar_t* wcschr(wchar_t*, wchar_t);
const wchar_t* wcsrchr(const wchar_t*, wchar_t);
wchar_t* wcsrchr(wchar_t*, wchar_t);
const wchar_t* wcsstr(const wchar_t*, const wchar_t*);
wchar_t* wcsstr(wchar_t*, const wchar_t*);
long wcstol(const wchar_t*, wchar_t**, int);
unsigned long wcstoul(const wchar_t*, wchar_t**, int);
size_t mbsrtowcs(wchar_t*, const char**, size_t, mbstate_t*);
size_t wcsrtombs(char*, const wchar_t**, size_t, mbstate_t*);
size_t wcstombs(char*, const wchar_t*, size_t);
size_t mbstowcs(wchar_t*, const char*, size_t);
int vswprintf(wchar_t*, size_t, const wchar_t*, va_list);
int fwprintf(FILE*, const wchar_t*, ...);
}
