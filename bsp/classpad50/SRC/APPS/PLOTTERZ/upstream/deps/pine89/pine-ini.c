/**
 * @file        pine-ini.c
 * @brief       Implementation of the pine-ini C89 INI file parser.
 *
 * Provides parsing, querying, and memory management routines for INI
 * configuration files. All public API declarations are in pine-ini.h.
 *
 * @author      pine89
 */

#include <stdlib.h>
#include <string.h>
#include "pine-ini.h"

#ifndef NULL
#define NULL ((void *) 0)
#endif

/** Internal boolean type. */
typedef int PINE_BOOL;

/** Boolean true value. */
#define PINE_TRUE   1

/** Boolean false value. */
#define PINE_FALSE  0

/* ------------------------------------------------------------------------- */
/*  Internal function declarations                                           */
/* ------------------------------------------------------------------------- */

PineIniParameter*   PineIni_Parameter_New       (const char* szKey, const char* szValue);
PineIniParameter*   PineIni_Parameter_Assign    (PineIniParameter* pParam, const char* szValue);
void                PineIni_Parameter_Destroy   (PineIniParameter* pParam);

PineIniSection*     PineIni_Section_New         (const char* pSectionName);
PineIniParameter*   PineIni_Section_Find        (const PineIniSection* pSection, const char* szKey);
PineIniSection*     PineIni_Section_Append      (PineIniSection* pSection, const char* szKey, const char* szValue);
void                PineIni_Section_Destroy     (PineIniSection* pSection);

char*   PineIni_ToSubstring        (char* src, int left, int right);
int     PineIni_FindChar           (const char* src, const char find);
char*   PineIni_Substring          (char* dest, const char* src, int left, int right);
char*   PineIni_StringDump         (const char* src);
char*   PineIni_StringTrim         (char* src);
char*   PineIni_StringRemoveQuotes (char* src);

/**
 * @brief  Case-sensitive string equality test.
 * @param  str1  First string to compare.
 * @param  str2  Second string to compare.
 * @return Non-zero (true) if equal, zero (false) otherwise.
 */
#define PineIni_StringEquals(str1, str2) (strcmp((str1), (str2)) == 0)

/**
 * @brief  Tests whether a character is whitespace.
 * @param  c  The character to test.
 * @return Non-zero (true) if c is space, CR, LF, or tab.
 */
#define isWhitespace(c) ((c) == ' ' || c == '\r' || c == '\n' || c == '\t')

/** Table of error message strings indexed by PineErrorCode. */
static const char* PINE_INI_ERRMSG[] = {
    "No error",
    "Illegal pattern",
    "Empty szKey",
    "Sections exceed",
    "Parameters exceed"
};

/**
 * @brief  Translates a PineErrorCode into a human-readable string.
 * @param  iErrorCode  The error code to look up.
 * @return Pointer to a static string, or "N/A" for unknown codes.
 */
const char* PintIni_GetErrorMessage(PineErrorCode iErrorCode) {
    switch (iErrorCode){
        case PINE_INI_ERRCODE_NO:
        case PINE_INI_ERRCODE_ILLEGAL_PATTERN:
        case PINE_INI_ERRCODE_EMPTY_KEY:
        case PINE_INI_ERRCODE_SECTION_EXCEED:
        case PINE_INI_ERRCODE_PARAMS_EXCEED:
            return PINE_INI_ERRMSG[iErrorCode];
        default:
            return "N/A";
    }
}

/** Parser action: continue reading lines. */
#define PARSER_ACTION_CONTINUE  1

/** Parser action: end of input reached. */
#define PARSER_ACTION_END       0

/**
 * @brief Internal state of the line-by-line INI parser.
 */
typedef struct tagPineIniParser {
    const char * szTextBuffer;    /**< Pointer to the full input buffer.         */
    const char * currentPointer;  /**< Current read position within the buffer.  */
    int iLineNumber;              /**< Current line number (1-based).            */
} PineIniParser;

/**
 * @brief  Allocates and initialises a new PineIniParser.
 * @param  szTextBuffer  Zero-terminated INI text buffer.
 * @return Pointer to the newly allocated parser, or NULL on allocation failure.
 */
static PineIniParser* PineIni_Parser_New(const char* szTextBuffer) {
    PineIniParser* pParser = (PineIniParser *)malloc(sizeof(PineIniParser));

    pParser->szTextBuffer   = szTextBuffer;
    pParser->currentPointer = pParser->szTextBuffer;
    pParser->iLineNumber    = 0;

    return pParser;
}

/**
 * @brief  Frees a PineIniParser and its associated memory.
 * @param  pParser  Pointer to the parser to destroy. Safe to pass NULL.
 */
static void PineIni_Parser_Destroy(PineIniParser* pParser) {
    if (pParser) {
        free(pParser);
    }
}

/**
 * @brief  Extracts the next line from the parser's text buffer.
 * @param  pParser  Pointer to the parser state.
 * @param  szBuf    Output buffer to receive the line (must be pre-allocated).
 * @return PARSER_ACTION_CONTINUE if more lines remain,
 *         PARSER_ACTION_END if EOF has been reached.
 * @note   The newline character is not copied into the output buffer.
 */
static int PineIni_Parser_GetLine(PineIniParser* pParser, char* szBuf) {
    char *pBuf = szBuf;

    pParser->iLineNumber++;

    while (*pParser->currentPointer != '\n' && *pParser->currentPointer != '\0') {
        *(pBuf++) = *(pParser->currentPointer++);
    }

    *pBuf = '\0';

    if (*pParser->currentPointer == '\0') {
        return PARSER_ACTION_END;
    }
    else {
        /* Skip LF */
        pParser->currentPointer++;
        return PARSER_ACTION_CONTINUE;
    }
}

/**
 * @brief  Checks whether a line is empty or consists only of whitespace.
 * @param  szLine   The line buffer.
 * @param  iLength  Length of the line.
 * @return PINE_TRUE if the line is considered empty, PINE_FALSE otherwise.
 */
static PINE_BOOL PineIni_Line_Empty(const char* szLine, const int iLength) {
    if (iLength <= 0 || PineIni_StringEquals("", szLine)) {
        return PINE_TRUE;
    }
    return PINE_FALSE;
}

/**
 * @brief  Checks whether a line is a comment (starts with ';').
 * @param  szLine   The line buffer.
 * @param  iLength  Length of the line.
 * @return PINE_TRUE if the line is a comment, PINE_FALSE otherwise.
 */
static PINE_BOOL PineIni_Line_IsComment(const char* szLine, const int iLength) {
    if (iLength >= 1 && szLine[0] == ';') {
        return PINE_TRUE;
    }
    return PINE_FALSE;
}

/**
 * @brief  Checks whether a line is a section header, e.g. `[mysection]`.
 * @param  szLine   The line buffer.
 * @param  iLength  Length of the line.
 * @return PINE_TRUE if the line is a section header, PINE_FALSE otherwise.
 */
static PINE_BOOL PineIni_Line_IsSection(const char* szLine, const int iLength) {
    if (iLength >= 2 && szLine[0] == '[' && szLine[iLength - 1] == ']') {
        return PINE_TRUE;
    }
    return PINE_FALSE;
}

/**
 * @brief  Checks whether a line is a key-value parameter (contains '=').
 * @param  szLine   The line buffer.
 * @param  iLength  Length of the line.
 * @return PINE_TRUE if the line is a parameter, PINE_FALSE otherwise.
 */
static PINE_BOOL PineIni_Line_IsParameter(const char* szLine, const int iLength) {
    int i;
    for (i = 0; i < iLength; ++i) {
        if (szLine[i] == '=') {
            return PINE_TRUE;
        }
    }
    return PINE_FALSE;
}

/* ------------------------------------------------------------------------- */
/*  Parse API                                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief  Macro to record an error, clean up, and return NULL from the parser.
 * @param  error_code  The PineErrorCode to report.
 */
#define PARSER_RETURN_ERROR(error_code)                                            \
                                    pErrorRet->iLineNumber = pParser->iLineNumber;   \
                                    pErrorRet->iErrorCode  = (error_code);           \
                                    strcpy(pErrorRet->szLineContent, szLineBuffer);    \
                                    PineIni_Parser_Destroy(pParser);                \
                                    PineIni_Destroy(pIniResult);                     \
                                    return NULL

/**
 * @brief  Parses a zero-terminated INI text buffer into a PineIniFile structure.
 * @param  szIniText   Zero-terminated string containing the full INI content.
 * @param  pErrorRet   Pointer to a PineIniError to be filled on failure.
 * @return Pointer to a newly allocated PineIniFile on success, or NULL on error.
 *
 * The parser processes the input line by line. Each line may be:
 * - An empty / whitespace-only line  (skipped)
 * - A comment starting with `;`       (skipped)
 * - A section header `[name]`         (switches current section)
 * - A key-value pair `key=value`      (added to the current section)
 *
 * A default section named `_default` is created automatically and used as
 * the active section until the first `[section]` header is encountered.
 *
 * @note   The caller is responsible for freeing the returned object via
 *         PineIni_Destroy().
 */
PineIniFile* PineIni_Parse(const char* szIniText, PineIniError* pErrorRet) {
    char            szLineBuffer[PINE_INI_LINE_MAX_LEN];
    int             iLineLength;
    int             iLineRetCode;
    PineIniParser*  pParser;
    PineIniFile*    pIniResult;
    PineIniSection* pCurrentSection;

    pParser = PineIni_Parser_New(szIniText);

    /* Create pIniResult */
    pIniResult = (PineIniFile *)malloc(sizeof(PineIniFile));
    pIniResult->iNumSection = 0;

    /* Create a default pSection for pFile */
    /* and append to pIniResult */
    pCurrentSection = PineIni_Section_New("_default");
    PineIni_Append(pIniResult, pCurrentSection);

    /* Clear error info */
    pErrorRet->iLineNumber  = 0;
    pErrorRet->iErrorCode   = PINE_INI_ERRCODE_NO;
    strcpy(pErrorRet->szLineContent, "");

    do {
        /* Get szLine from parser */
        iLineRetCode = PineIni_Parser_GetLine(pParser, szLineBuffer);

        /* Trim white spaces */
        PineIni_StringTrim(szLineBuffer);

        /* Check pattern */
        iLineLength = strlen(szLineBuffer);

        /* Pattern : empty */
        if (PineIni_Line_Empty(szLineBuffer, iLineLength)) {
            continue;
        }
        /* Pattern : comment */
        else if (PineIni_Line_IsComment(szLineBuffer, iLineLength)) {
            continue;
        }
        /* Pattern : section */
        else if (PineIni_Line_IsSection(szLineBuffer, iLineLength)) {
            char            pSectionName[PINE_INI_LINE_MAX_LEN];
            PineIniSection* pSection;

            PineIni_Substring(pSectionName, szLineBuffer, 1, iLineLength - 2);

            pSection = PineIni_Find(pIniResult, pSectionName);

            /* Section szName not exist in pIniResult */
            if (pSection == NULL) {
                /* Check if exceed */
                if (pIniResult->iNumSection >= PINE_INI_MAX_NUM_SECTIONS) {
                    PARSER_RETURN_ERROR(PINE_INI_ERRCODE_SECTION_EXCEED);
                }

                /* Create a new pSection and append in pIniResult */
                pSection = PineIni_Section_New(pSectionName);
                PineIni_Append(pIniResult, pSection);
            }

            pCurrentSection = pSection;

            continue;
        }
        /* Pattern : parameter */
        else if (PineIni_Line_IsParameter(szLineBuffer, iLineLength)) {
            char                szKey[PINE_INI_LINE_MAX_LEN];
            char                szValue[PINE_INI_LINE_MAX_LEN];
            int                 equalPos;
            PineIniParameter*   pParam;

            /* Get key and value */
            equalPos = PineIni_FindChar(szLineBuffer, '=');
            PineIni_Substring(szKey, szLineBuffer, 0, equalPos - 1);
            PineIni_Substring(szValue, szLineBuffer, equalPos + 1, iLineLength - 1);
            PineIni_StringTrim(szKey);
            PineIni_StringRemoveQuotes(szValue);

            /* Error: empty key */
            if (strlen(szKey) <= 0) {
                PARSER_RETURN_ERROR(PINE_INI_ERRCODE_EMPTY_KEY);
            }

            /* Check if key already exist */
            pParam = PineIni_Section_Find(pCurrentSection, szKey);

            /* Exist, update value */
            if (pParam) {
                PineIni_Parameter_Assign(pParam, szValue);
            }
            /* Not exist, append to current section */
            else {
                /* Check if exceed */
                if (pCurrentSection->iNumParam >= PINE_INI_MAX_NUM_PARAMETERS) {
                    PARSER_RETURN_ERROR(PINE_INI_ERRCODE_PARAMS_EXCEED);
                }
                PineIni_Section_Append(pCurrentSection, szKey, szValue);
            }

            continue;
        }
        /* Illegal Pattern! */
        else {
            PARSER_RETURN_ERROR(PINE_INI_ERRCODE_ILLEGAL_PATTERN);
        }

    } while(iLineRetCode != PARSER_ACTION_END);

    PineIni_Parser_Destroy(pParser);

    return pIniResult;
}

/* ------------------------------------------------------------------------- */
/*  Public API                                                               */
/* ------------------------------------------------------------------------- */

/**
 * @brief  Finds a section by name within a parsed INI file.
 * @param  pFile          Pointer to the parsed PineIniFile.
 * @param  pSectionName   Name of the section to locate.
 * @return Pointer to the PineIniSection if found, or NULL otherwise.
 */
PineIniSection* PineIni_Find(const PineIniFile* pFile, const char* pSectionName) {
    if (pFile) {
        int i;
        for (i = 0; i < pFile->iNumSection; ++i) {
            PineIniSection* pSection = pFile->pSections[i];
            if (PineIni_StringEquals(pSectionName, pSection->szName)) {
                return pSection;
            }
        } 
    }
    return NULL;
}

/**
 * @brief  Appends a section to the INI file container.
 * @param  pFile     Pointer to the PineIniFile to modify.
 * @param  pSection  Pointer to the PineIniSection to append.
 * @return Pointer to the modified PineIniFile (same as @p pFile).
 */
PineIniFile* PineIni_Append(PineIniFile* pFile, PineIniSection* pSection) {
    if (pFile) {
        pFile->pSections[pFile->iNumSection++] = pSection;
    }
    return pFile;
}

/**
 * @brief  Frees all memory associated with a parsed PineIniFile.
 * @param  pFile  Pointer to the PineIniFile to destroy. Safe to pass NULL.
 */
void PineIni_Destroy(PineIniFile* pFile) {
    if (pFile) {
        int i;

        /* destory pSections in pFile */
        for (i = 0; i < pFile->iNumSection; ++i) {
            PineIni_Section_Destroy(pFile->pSections[i]);
        }

        /* free pFile self */
        free(pFile);
    }
}

/* ------------------------------------------------------------------------- */
/*  Section management                                                       */
/* ------------------------------------------------------------------------- */

/**
 * @brief  Allocates and initialises a new PineIniSection.
 * @param  pSectionName  Name for the new section (will be copied).
 * @return Pointer to the newly allocated section, or NULL on allocation failure.
 */
PineIniSection* PineIni_Section_New(const char* pSectionName) {
    PineIniSection* pSection = (PineIniSection *)malloc(sizeof(PineIniSection));

    pSection->szName    = PineIni_StringDump(pSectionName);
    pSection->iNumParam = 0;

    return pSection;
}

/**
 * @brief  Searches for a parameter by key within a section.
 * @param  pSection  Pointer to the PineIniSection.
 * @param  szKey     The key to search for.
 * @return Pointer to the PineIniParameter if found, or NULL otherwise.
 */
PineIniParameter* PineIni_Section_Find(const PineIniSection* pSection, const char* szKey) {
    if (pSection) {
        int i;
        for (i = 0; i < pSection->iNumParam; ++i) {
            PineIniParameter* pParam = pSection->pParams[i];
            if (PineIni_StringEquals(szKey, pParam->szKey)) {
                return pParam;
            }
        }
    }
    return NULL;
}

/**
 * @brief  Appends a new key-value parameter to a section.
 * @param  pSection  Pointer to the PineIniSection to modify.
 * @param  szKey     The key string (will be copied).
 * @param  szValue   The value string (will be copied).
 * @return Pointer to the modified section (same as @p pSection).
 */
PineIniSection* PineIni_Section_Append(PineIniSection* pSection, const char* szKey, const char* szValue) {
    if (pSection) {
        pSection->pParams[pSection->iNumParam++] = PineIni_Parameter_New(szKey, szValue);
    }
    return pSection;
}

/**
 * @brief  Frees all memory associated with a PineIniSection.
 * @param  pSection  Pointer to the section to destroy. Safe to pass NULL.
 */
void PineIni_Section_Destroy(PineIniSection* pSection) {
    if (pSection) {
        int i;

        /* free pSection szName */
        if (pSection->szName) {
            free(pSection->szName);
        }
        /* destory parameters in pSection */
        for (i = 0; i < pSection->iNumParam; ++i) {
            PineIni_Parameter_Destroy(pSection->pParams[i]);
        }
        /* free pSection self */
        free(pSection);
    }
}

/* ------------------------------------------------------------------------- */
/*  Parameter management                                                     */
/* ------------------------------------------------------------------------- */

/**
 * @brief  Allocates and initialises a new PineIniParameter.
 * @param  szKey   The key string (will be copied).
 * @param  szValue The value string (will be copied).
 * @return Pointer to the newly allocated parameter, or NULL on allocation failure.
 */
PineIniParameter* PineIni_Parameter_New(const char* szKey, const char* szValue) {
    PineIniParameter* pParam;

    pParam          = (PineIniParameter *)malloc(sizeof(PineIniParameter));
    pParam->szKey   = PineIni_StringDump(szKey);
    pParam->szValue = PineIni_StringDump(szValue);

    return pParam;
}

/**
 * @brief  Replaces the value of an existing parameter.
 * @param  pParam   Pointer to the parameter to modify.
 * @param  szValue  The new value string (will be copied).
 * @return Pointer to the modified parameter (same as @p pParam).
 * @note   The old value is freed before the new value is assigned.
 */
PineIniParameter* PineIni_Parameter_Assign(PineIniParameter* pParam, const char* szValue) {
    if (pParam) {
        if (pParam->szValue) {
            free(pParam->szValue);
        }
        pParam->szValue = PineIni_StringDump(szValue);
    }
    return pParam;
}

/**
 * @brief  Frees all memory associated with a PineIniParameter.
 * @param  pParam  Pointer to the parameter to destroy. Safe to pass NULL.
 */
void  PineIni_Parameter_Destroy(PineIniParameter* pParam) {
    if (pParam) {
        if (pParam->szKey) {
            free(pParam->szKey);
        }
        if (pParam->szValue) {
            free(pParam->szValue);
        }
        free(pParam);
    }
}

/* ------------------------------------------------------------------------- */
/*  Utility functions                                                        */
/* ------------------------------------------------------------------------- */

/**
 * @brief  Retrieves a string value from a section by key.
 * @param  pSection       Pointer to the PineIniSection.
 * @param  szKey          The key to search for.
 * @param  szDefaultValue Value returned when the key is not found.
 * @return Pointer to the value string, or @p szDefaultValue if not found.
 */
const char* PineIni_Section_GetString(const PineIniSection* pSection, const char* szKey, const char* szDefaultValue) {
    const PineIniParameter* pParam = PineIni_Section_Find(pSection, szKey);

    /* szKey not found or szValue is null */
    if (!pParam || !pParam->szValue) {
        return szDefaultValue;
    }

    return pParam->szValue;
}

/**
 * @brief  Retrieves a string value from an INI file by section and key.
 * @param  pFile          Pointer to the parsed PineIniFile.
 * @param  pSectionName   Name of the section to search.
 * @param  szKey          The key to search for.
 * @param  szDefaultValue Value returned when the section or key is not found.
 * @return Pointer to the value string, or @p szDefaultValue if not found.
 */
const char* PineIni_GetString(const PineIniFile* pFile, const char* pSectionName, const char* szKey, const char* szDefaultValue) {
    const PineIniSection* pSection = PineIni_Find(pFile, pSectionName);

    /* pSectionName not found */
    if (!pSection) {
        return szDefaultValue;
    }

    return PineIni_Section_GetString(pSection, szKey, szDefaultValue);
}

/**
 * @brief  Retrieves an integer value from a section by key.
 * @param  pSection       Pointer to the PineIniSection.
 * @param  szKey          The key to search for.
 * @param  iDefaultValue    Value returned when the key is not found.
 * @return The parsed integer value, or @p szDefaultValue if not found.
 * @note   Uses atol() internally; no overflow checking is performed.
 */
int PineIni_Section_GetInt(const PineIniSection* pSection, const char* szKey, int iDefaultValue) {
    const char* szIntVal = PineIni_Section_GetString(pSection, szKey, NULL);

    if (szIntVal == NULL) {
        return iDefaultValue;
    } else {
        return atol(szIntVal);
    }
}

/**
 * @brief  Retrieves an integer value from an INI file by section and key.
 * @param  pFile          Pointer to the parsed PineIniFile.
 * @param  pSectionName   Name of the section to search.
 * @param  szKey          The key to search for.
 * @param  iDefaultValue    Value returned when the section or key is not found.
 * @return The parsed integer value, or @p iDefaultValue if not found.
 * @note   Uses atol() internally; no overflow checking is performed.
 */
int PineIni_GetInt(const PineIniFile* pFile, const char* pSectionName, const char* szKey, int iDefaultValue) {
    const char* szIntVal = PineIni_GetString(pFile, pSectionName, szKey, NULL);

    if (szIntVal == NULL) {
        return iDefaultValue;
    } else {
        return atol(szIntVal);
    }
}

/* ------------------------------------------------------------------------- */
/*  String helper functions                                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief  Finds the first occurrence of a character in a string.
 * @param  src   The string to search.
 * @param  find  The character to locate.
 * @return The zero-based index of the character, or -1 if not found.
 */
int PineIni_FindChar(const char* src, const char find) {
    int i;
    for (i = 0; src[i]; ++i) {
        if (src[i] == find) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief  Copies a substring from src to dest using inclusive indices [left, right].
 * @param  dest   Destination buffer (must be large enough to hold the result).
 * @param  src    Source string.
 * @param  left   Start index (inclusive).
 * @param  right  End index (inclusive).
 * @return Pointer to @p dest with the substring zero-terminated.
 */
char* PineIni_Substring(char* dest, const char* src, int left, int right) {
    char*   szBuf         = dest;
    int     newLength   = 0;
    int     i;

    for (i = left; i <= right; ++i) {
        szBuf[newLength++] = src[i]; 
    }

    szBuf[newLength] = 0;

    return dest;
}

/**
 * @brief  Replaces src in-place with its substring [left, right].
 * @param  src    The string to modify (must be writable).
 * @param  left   Start index (inclusive).
 * @param  right  End index (inclusive).
 * @return Pointer to @p src after modification.
 */
char* PineIni_ToSubstring (char* src, int left, int right) {
    int     iLength      = strlen(src);
    char*   szBuf         = (char *)malloc(iLength + 1);
    int     newLength   = 0;
    int     i;

    for (i = left; i <= right; ++i) {
        szBuf[newLength++] = src[i]; 
    }

    szBuf[newLength] = 0;

    strcpy(src, szBuf);

    free(szBuf);

    return src;
}

/**
 * @brief  Creates a heap-allocated copy of a string.
 * @param  src  The string to duplicate.
 * @return Pointer to the newly allocated copy, or NULL on allocation failure.
 * @note   The caller is responsible for freeing the returned pointer.
 */
char* PineIni_StringDump (const char* src) {
    int     iLength = strlen(src);
    char*   dup = (char *)malloc(iLength + 1);
    strcpy(dup, src);
    return dup;
}

/**
 * @brief  Trims leading and trailing whitespace from a string in-place.
 * @param  src  The string to trim (must be writable).
 * @return Pointer to @p src after trimming.
 */
char* PineIni_StringTrim(char* src) {
    int     iLength = strlen(src);
    int     left;
    int     right;

    for (left = 0; left < iLength; ++left) {
        if (!isWhitespace(src[left])) {
            break;
        }
    }

    for (right = iLength - 1; right >= 0; --right) {
        if (!isWhitespace(src[right])) {
            break;
        }
    }

    return PineIni_ToSubstring(src, left, right);
}

/**
 * @brief  Removes surrounding quotes (single or double) from a string in-place.
 * @param  src  The string to process (must be writable).
 * @return Pointer to @p src after quote removal.
 * @note   The string is first trimmed of whitespace. If the resulting string
 *         is at least 2 characters long and starts and ends with the same
 *         quote character (' or "), the quotes are stripped.
 */
char* PineIni_StringRemoveQuotes(char* src) {
    int     iLength;
    int     isSingleQuotesAround;
    int     isDoubleQuotesAround;


    PineIni_StringTrim(src);

    /* Too short, no need to check */
    if ((iLength  = strlen(src)) < 2) {
        return src;
    }

    isSingleQuotesAround = (src[0] == '"' && src[iLength - 1] == '"');
    isDoubleQuotesAround = (src[0] == '\'' && src[iLength - 1] == '\'');

    if (!isSingleQuotesAround && !isDoubleQuotesAround) {
        return src;
    }

    return PineIni_ToSubstring(src, 1, iLength - 2);
}
