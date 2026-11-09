#ifndef _EVALUATOR_Z_
#define _EVALUATOR_Z_

/* Workaround for fx-9860 SHC compiler's inability to parse paths correctly */
#if defined(_SH3) || defined(_SH4)
#   ifndef PLATFORM_FX9860
#       define PLATFORM_FX9860
#   endif
#endif

#ifdef PLATFORM_FX9860
#   include "../../formula-z/fz.h"
#else
#   include "../formula-z/fz.h"
#endif

/*================================================
 * Common
 *------------------------------------------------
 * 
 *================================================*/

typedef enum {
    EZOP_NOP = 0,
    EZOP_NEG,
    /* Basic: + , - , * , / , ^ */
    EZOP_ADD, EZOP_SUB, EZOP_MUL, EZOP_DIV, EZOP_POW,
    /* Integer: INTDIV , MOD */
    EZOP_INTDIV, EZOP_MOD,
    /* Logic: ! , && , ||*/
    EZOP_NOT, EZOP_AND, EZOP_OR,
    /* Comparison */
    EZOP_EQUAL, EZOP_NEQ, EZOP_GT, EZOP_LT,
    EZOP_GTEQ, EZOP_LTEQ,
    /* Push immediate data / variable */
    EZOP_PUSH_IMD, EZOP_PUSH_VAR,
    /* Function call */
    EZOP_FUNC
} EzOpCode;

typedef struct {
    EzOpCode iOpCode;
    union {
        PZ_FLOAT    fImmediate;
        int         iVarIndex;
        int         iFuncIndex;
        int         iNop;
    } uData;
} EzInstruction;

const char* EzOpCode_GetName(EzOpCode iCode);

typedef struct tagEzMachine {
    /* Variables */
    Vlist*          pListVariableName; /* char* */
    PZ_FLOAT*       pVariableValueArray;
    int             iVariableLength;

    /* Operand Stack */
    int             iStackLength;
    int             iStackSize;
    PZ_FLOAT*       pStack;

    /* Instructions */
    int             iInstructionLength;
    int             iInstructionCur;
    EzInstruction*  pInstructions;
} EzMachine;

/* EzMachine for Interval Arithmetic */

typedef struct tagEzInterval {
    PZ_FLOAT fLow, fHigh;
} EzInterval;

#define IntervalSet(ivlDest, ivlSrc) (((ivlDest).fLow = (ivlSrc).fLow), ((ivlDest).fHigh = (ivlSrc).fHigh))

typedef struct tagEzIaMachine {
    EzInterval*     pVariableValueArray;
    int             iVariableLength;

    /* Operand Stack */
    int             iStackLength;
    int             iStackSize;
    EzInterval*     pStack;

    /* Instructions */
    int             iInstructionLength;
    const EzInstruction* pInstructions;
} EzIaMachine;

/*================================================
 * Emitter
 *------------------------------------------------
 * 
 *================================================*/

#define EZ_ERROR_CONTENT_LENGTH 100

typedef enum {
    EZERR_NONE = 0,
    EZERR_VARIABLE_UNDEFINED,
    EZERR_FUNCTION_UNDEFINED,
    EZERR_FUNCTION_PARAM_MISMATCH
} EzError;

EzError             EzMachine_Compile               (EzMachine* pVm, const FzAstNode* pAstExpr, char* szErrorContent);

/*================================================
 * Evaluator
 *------------------------------------------------
 * 
 *================================================*/

EzMachine*          EzMachine_Create                ();
void                EzMachine_Destroy               (EzMachine* pVm);
BOOL                EzMachine_DeclareVariable       (EzMachine* pVm, const char* szVarName);
void                EzMachine_AllocateVariables     (EzMachine* pVm);
int                 EzMachine_GetVariableIndexByName(EzMachine* pVm, const char* szVarName);
int                 EzMachine_GetVariableIndexByView(EzMachine* pVm, const StringView* pStrView);
void                EzMachine_SetVariableByIndex    (EzMachine* pVm, int iIndex, PZ_FLOAT fValue);
PZ_FLOAT            EzMachine_GetVariableByIndex    (EzMachine* pVm, int iIndex);
PZ_FLOAT            EzMachine_Eval                  (EzMachine* pVm);

EzIaMachine*        EzIaMachine_CreateFrom          (const EzMachine* pVm);
void                EzIaMachine_Destroy             (EzIaMachine* pIaVm);
void                EzIaMachine_SetVariableByIndex  (EzIaMachine* pIaVm, int iIndex, PZ_FLOAT fLow, PZ_FLOAT fHigh);
EzInterval*         EzIaMachine_Eval                (EzIaMachine* pIaVm, EzInterval* pIvlOut);


/* If building with CodeWarrior for Palm OS */
#if defined(__MWERKS__) && (defined(__MC68K__) || defined(__PALMOS_TRAPS__))
/* Manually declare necessary math functions. They are called in MathLib.c */
double acos(double x);
double asin(double x);
double atan(double x);
double cos(double x);
double sin(double x);
double tan(double x);
double pow(double x, double y);
double sqrt(double x);
double log(double x);
double exp(double x);
double fabs(double x);

double floor(double x);
double ceil(double x);
#endif

#endif