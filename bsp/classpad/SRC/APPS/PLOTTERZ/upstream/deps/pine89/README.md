# Pine89

A lightweight INI parser for embedded systems, written in ANSI C (C89).

## Features

- Sections (`[section]`) and key-value pairs (`key=value`)
- Comment lines starting with `;`
- Quoted values (single and double quotes)
- Automatic `_default` section for top-level keys
- Duplicate sections are merged; duplicate keys overwrite
- Compile-time limits: 25 sections, 25 parameters per section, 150 chars/line

## API

See `pine-ini.h` for full documentation.

```c
PineIniFile*    PineIni_Parse(const char* szIniText, PineIniError* pErrorRet);
PineIniSection* PineIni_Find(const PineIniFile* pFile, const char* pSectionName);
PineIniFile*    PineIni_Append(PineIniFile* pFile, PineIniSection* pSection);
void            PineIni_Destroy(PineIniFile* pFile);
const char*     PintIni_GetErrorMessage(PineErrorCode iErrorCode);

const char*     PineIni_Section_GetString(const PineIniSection* pSection,
                     const char* szKey, const char* szDefaultValue);
const char*     PineIni_GetString(const PineIniFile* pFile,
                     const char* pSectionName, const char* szKey,
                     const char* szDefaultValue);
int             PineIni_Section_GetInt(const PineIniSection* pSection,
                     const char* szKey, int iDefaultValue);
int             PineIni_GetInt(const PineIniFile* pFile,
                     const char* pSectionName, const char* szKey,
                     int iDefaultValue);
```

## Build

```
gcc -std=c89 -Wall -o myapp pine-ini.c myapp.c
```

## Test

CLI test driver (`pine-test.c`):

```
gcc -std=c89 -Wall -o pine-test pine-ini.c pine-test.c

# Validate INI syntax (prints error code, 0 = ok)
./pine-test "hello=1" -a validate

# Get a value
./pine-test "hello=1" -a get -s _default -k hello -d "fallback"
```

Python 3.4+ test suite:

```
python pine-test.py
```

Generates `pine-test-report.html` with pass/fail results.

## Files

| File | Description |
|---|---|
| `pine-ini.h` | Public API header |
| `pine-ini.c` | Parser implementation |
| `pine-test.c` | CLI test driver |
| `pine-test.py` | Python test suite / HTML report generator |
| `README.md` | This file |

## License

MIT
