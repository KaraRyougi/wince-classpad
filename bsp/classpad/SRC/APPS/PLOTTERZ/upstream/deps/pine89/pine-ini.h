/**
 * @file        pine-ini.h
 * @brief       Lightweight INI file parser for embedded systems (C89).
 *
 * This library provides a minimal, malloc-based INI parser designed for
 * resource-constrained environments. It supports sections, key-value pairs,
 * comments (lines starting with `;`), and quoted values.
 *
 * @note        All limits are compile-time configurable via the macros below.
 * @note        The parser automatically creates a `_default` section for
 *              key-value pairs that appear before any explicit `[section]`.
 *
 * @author      pine89
 */

#ifndef _PINE_INI_H_
#define _PINE_INI_H_

/** Maximum number of parameters (key-value pairs) allowed per section. */
#define PINE_INI_MAX_NUM_PARAMETERS     25

/** Maximum number of sections allowed in a single INI file. */
#define PINE_INI_MAX_NUM_SECTIONS       25

/** Maximum length (in characters) of a single line in the INI file. */
#define PINE_INI_LINE_MAX_LEN          150

/**
 * @brief Represents a single key-value parameter within an INI section.
 */
typedef struct tagPineIniParameter {
    char* szKey;    /**< Pointer to the parameter key string.   */
    char* szValue;  /**< Pointer to the parameter value string. */
} PineIniParameter;

/**
 * @brief Represents a section in an INI file, holding an array of parameters.
 */
typedef struct tagPineIniSection {
    char*           szName;     /**< Pointer to the section name string.                  */
    int             iNumParam;  /**< Number of parameters currently stored.               */
    PineIniParameter* pParams[PINE_INI_MAX_NUM_PARAMETERS]; /**< Array of parameter pointers. */
} PineIniSection;

/**
 * @brief Top-level container for a parsed INI file.
 */
typedef struct tagPineIni {
    int             iNumSection;                                /**< Number of sections currently stored. */
    PineIniSection* pSections[PINE_INI_MAX_NUM_SECTIONS];       /**< Array of section pointers.           */
} PineIniFile;

/**
 * @brief Error codes returned by the parser.
 */
typedef enum {
    PINE_INI_ERRCODE_NO = 0,            /**< No error occurred.                        */
    PINE_INI_ERRCODE_ILLEGAL_PATTERN,   /**< Line does not match any valid pattern.    */
    PINE_INI_ERRCODE_EMPTY_KEY,         /**< A parameter has an empty key.             */
    PINE_INI_ERRCODE_SECTION_EXCEED,    /**< Maximum number of sections exceeded.      */
    PINE_INI_ERRCODE_PARAMS_EXCEED      /**< Maximum number of parameters exceeded.    */
} PineErrorCode;

/**
 * @brief Detailed error information produced by PineIni_Parse().
 */
typedef struct tagPineIniError {
    int    iErrorCode;                              /**< Error code (@see PineErrorCode).                */
    int    iLineNumber;                             /**< Line number where the error was detected.        */
    char   szLineContent[PINE_INI_LINE_MAX_LEN];    /**< Content of the offending line (zero-terminated). */
} PineIniError;

/**
 * @brief  Returns a human-readable error message for a given error code.
 * @param  iErrorCode  The error code to translate.
 * @return Pointer to a static string describing the error.
 */
const char* PintIni_GetErrorMessage(PineErrorCode iErrorCode);

/**
 * @brief  Parses a zero-terminated INI text buffer into a PineIniFile structure.
 * @param  iniText   Zero-terminated string containing the full INI content.
 * @param  errorRet  Pointer to a PineIniError to be filled on failure.
 * @return Pointer to a newly allocated PineIniFile on success, or NULL on error.
 * @note   The caller is responsible for freeing the returned object via
 *         PineIni_Destroy().
 */
PineIniFile*        PineIni_Parse               (const char* szIniText, PineIniError* pErrorRet);

/**
 * @brief  Finds a section by name within a parsed INI file.
 * @param  file         Pointer to the parsed PineIniFile.
 * @param  sectionName  Name of the section to locate.
 * @return Pointer to the PineIniSection if found, or NULL otherwise.
 */
PineIniSection*     PineIni_Find                (const PineIniFile* pFile, const char* pSectionName);

/**
 * @brief  Appends a section to the INI file container.
 * @param  file     Pointer to the PineIniFile to modify.
 * @param  section  Pointer to the PineIniSection to append.
 * @return Pointer to the modified PineIniFile (same as @p file).
 */
PineIniFile*        PineIni_Append              (PineIniFile* pFile, PineIniSection* pSection);

/**
 * @brief  Frees all memory associated with a parsed PineIniFile.
 * @param  file  Pointer to the PineIniFile to destroy. Safe to pass NULL.
 */
void                PineIni_Destroy             (PineIniFile* pFile);

/* ------------------------------------------------------------------------- */
/*  Utility / convenience functions                                          */
/* ------------------------------------------------------------------------- */

/**
 * @brief  Retrieves a string value from a section by key.
 * @param  section       Pointer to the PineIniSection.
 * @param  szKey         The key to search for.
 * @param  defaultValue  Value returned when the key is not found.
 * @return Pointer to the value string, or @p defaultValue if not found.
 */
const char*     PineIni_Section_GetString   (const PineIniSection* pSection, const char* szKey, const char* szDefaultValue);

/**
 * @brief  Retrieves a string value from an INI file by section and key.
 * @param  file             Pointer to the parsed PineIniFile.
 * @param  sectionName      Name of the section to search.
 * @param  szKey            The key to search for.
 * @param  szDefaultValue   Value returned when the section or key is not found.
 * @return Pointer to the value string, or @p defaultValue if not found.
 */
const char*     PineIni_GetString           (const PineIniFile* file, const char* pSectionName, const char* szKey, const char* szDefaultValue);

/**
 * @brief  Retrieves an integer value from a section by key.
 * @param  section          Pointer to the PineIniSection.
 * @param  szKey            The key to search for.
 * @param  iDefaultValue    Value returned when the key is not found.
 * @return The parsed integer value, or @p iDefaultValue if not found.
 * @note   Uses atol() internally; no overflow checking is performed.
 */
int             PineIni_Section_GetInt      (const PineIniSection* pSection, const char* szKey, int iDefaultValue);

/**
 * @brief  Retrieves an integer value from an INI file by section and key.
 * @param  file             Pointer to the parsed PineIniFile.
 * @param  sectionName      Name of the section to search.
 * @param  szKey            The key to search for.
 * @param  iDefaultValue    Value returned when the section or key is not found.
 * @return The parsed integer value, or @p iDefaultValue if not found.
 * @note   Uses atol() internally; no overflow checking is performed.
 */
int             PineIni_GetInt              (const PineIniFile* file, const char* pSectionName, const char* szKey, int iDefaultValue);


#endif /* _PINE_INI_H_ */
