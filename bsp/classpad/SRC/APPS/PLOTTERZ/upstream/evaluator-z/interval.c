#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ez.h"
#include "../common/constants.h"

/* Use HUGE_VAL to represent infinity */
#ifndef PZ_INFINITY
#define PZ_INFINITY HUGE_VAL
#endif

/* Helper macros: min and max */
#define IA_MIN(a, b) ((a) < (b) ? (a) : (b))
#define IA_MAX(a, b) ((a) > (b) ? (a) : (b))
#define IA_MIN4(a, b, c, d) IA_MIN(IA_MIN(a, b), IA_MIN(c, d))
#define IA_MAX4(a, b, c, d) IA_MAX(IA_MAX(a, b), IA_MAX(c, d))

EzInterval* IA_Negative             (EzInterval*, EzInterval*);
EzInterval* IA_Add                  (EzInterval*, EzInterval*, EzInterval*);
EzInterval* IA_Subtract             (EzInterval*, EzInterval*, EzInterval*);
EzInterval* IA_Multiply             (EzInterval*, EzInterval*, EzInterval*);
EzInterval* IA_Divide               (EzInterval*, EzInterval*, EzInterval*);
EzInterval* IA_Power                (EzInterval*, EzInterval*, EzInterval*);
EzInterval* IA_IntegerDivide        (EzInterval*, EzInterval*, EzInterval*);
EzInterval* IA_Modulo               (EzInterval*, EzInterval*, EzInterval*);
EzInterval* IA_Not                  (EzInterval*, EzInterval*);
EzInterval* IA_And                  (EzInterval*, EzInterval*, EzInterval*);
EzInterval* IA_Or                   (EzInterval*, EzInterval*, EzInterval*);
EzInterval* IA_Equal                (EzInterval*, EzInterval*, EzInterval*);
EzInterval* IA_NotEqual             (EzInterval*, EzInterval*, EzInterval*);
EzInterval* IA_GreaterThan          (EzInterval*, EzInterval*, EzInterval*);
EzInterval* IA_LessThan             (EzInterval*, EzInterval*, EzInterval*);
EzInterval* IA_GreaterOrEqualThan   (EzInterval*, EzInterval*, EzInterval*);
EzInterval* IA_LessOrEqualThan      (EzInterval*, EzInterval*, EzInterval*);
EzInterval* IA_Arccosine            (EzInterval*, EzInterval*);
EzInterval* IA_Arcsine              (EzInterval*, EzInterval*);
EzInterval* IA_Arctangent           (EzInterval*, EzInterval*);
EzInterval* IA_Cosine               (EzInterval*, EzInterval*);
EzInterval* IA_Sine                 (EzInterval*, EzInterval*);
EzInterval* IA_Tangent              (EzInterval*, EzInterval*);
EzInterval* IA_SquareRoot           (EzInterval*, EzInterval*);
EzInterval* IA_Log                  (EzInterval*, EzInterval*);
EzInterval* IA_NaturalExponential   (EzInterval*, EzInterval*);
EzInterval* IA_Absolute             (EzInterval*, EzInterval*);

EzIaMachine* EzIaMachine_CreateFrom (const EzMachine* pVm) {
    EzIaMachine* pIaVm;

    if (pVm == NULL) {
        return NULL;
    }

    pIaVm = (EzIaMachine *)malloc(sizeof(EzIaMachine));
    if (pVm->iVariableLength > 0) {
        pIaVm->iVariableLength = pVm->iVariableLength;
        pIaVm->pVariableValueArray = (EzInterval *)malloc(sizeof(EzInterval) * pIaVm->iVariableLength);
    }
    else {
        pIaVm->pVariableValueArray = NULL;
        pIaVm->iVariableLength = 0;
    }

    if (pVm->iStackLength > 0) {
        pIaVm->iStackLength = pVm->iStackLength;
        pIaVm->iStackSize = pVm->iStackSize;
        pIaVm->pStack = (EzInterval *)malloc(sizeof(EzInterval) * pIaVm->iStackLength);
    }
    else {
        pIaVm->iStackLength = 0;
        pIaVm->iStackSize = 0;
        pIaVm->pStack = NULL;
    }

    pIaVm->pInstructions = pVm->pInstructions;
    pIaVm->iInstructionLength = pVm->iInstructionLength;

    return pIaVm;
}

void EzIaMachine_Destroy (EzIaMachine* pIaVm) {
    if (!pIaVm) return;
    if (pIaVm->pVariableValueArray) {
        free(pIaVm->pVariableValueArray); 
    }
    if (pIaVm->pStack) {
        free(pIaVm->pStack);
    }
    free(pIaVm);
}

void EzIaMachine_SetVariableByIndex (EzIaMachine* pIaVm, int iIndex, PZ_FLOAT fLow, PZ_FLOAT fHigh) {
    if (iIndex < 0 || iIndex >= pIaVm->iVariableLength) {
        return;
    }
    pIaVm->pVariableValueArray[iIndex].fLow = fLow;
    pIaVm->pVariableValueArray[iIndex].fHigh = fHigh;
}

#define IA_EvalPush(ivlSrc) do {                            \
    IntervalSet(pIaVm->pStack[pIaVm->iStackSize], ivlSrc);  \
    pIaVm->iStackSize++;                                    \
} while (0)

#define IA_EvalPop(ivlDesc) do {                            \
    --pIaVm->iStackSize;                                    \
    IntervalSet(ivlDesc, pIaVm->pStack[pIaVm->iStackSize]); \
} while(0)

#define IA_HandleSingleParamFunc(IA_MathFunc) do {  \
    IA_EvalPop(ivlArg);                             \
    IA_MathFunc(&ivlArg, &ivlResult);               \
    IA_EvalPush(ivlResult);                         \
    return;                                         \
} while(0)

static void EzAiMachine_Function (EzIaMachine* pIaVm, PzFuncIndex iFuncIndex) {
    EzInterval ivlArg, ivlResult;

    switch (iFuncIndex) {
        default:
        case PZF_NONE:  return;
        case PZF_SIN:   IA_HandleSingleParamFunc(IA_Sine);
        case PZF_COS:   IA_HandleSingleParamFunc(IA_Cosine);
        case PZF_TAN:   IA_HandleSingleParamFunc(IA_Tangent);
        case PZF_ASIN:  IA_HandleSingleParamFunc(IA_Arcsine);
        case PZF_ACOS:  IA_HandleSingleParamFunc(IA_Arccosine);
        case PZF_ATAN:  IA_HandleSingleParamFunc(IA_Arctangent);
        case PZF_SQR:   IA_HandleSingleParamFunc(IA_SquareRoot);
        case PZF_EXP:   IA_HandleSingleParamFunc(IA_NaturalExponential);
        case PZF_ABS:   IA_HandleSingleParamFunc(IA_Absolute);
        case PZF_LOG:   IA_HandleSingleParamFunc(IA_Log);
        case PZF_LN:    IA_HandleSingleParamFunc(IA_Log);
    }
}

EzInterval* EzIaMachine_Eval (EzIaMachine* pIaVm, EzInterval* pIvlOut) {
    int i;
    EzInterval ivlLeft, ivlRight, ivlResult;

    pIaVm->iStackSize = 0;
    
    for (i = 0; i < pIaVm->iInstructionLength; ++i) {
        const EzInstruction* pInst = pIaVm->pInstructions + i;
        switch (pInst->iOpCode) {
            default:
            case EZOP_NOP: break;
            case EZOP_NEG:
                IA_EvalPop(ivlLeft);
                IA_Negative(&ivlLeft, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_ADD:
                IA_EvalPop(ivlRight);
                IA_EvalPop(ivlLeft);
                IA_Add(&ivlLeft, &ivlRight, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_SUB:
                IA_EvalPop(ivlRight);
                IA_EvalPop(ivlLeft);
                IA_Subtract(&ivlLeft, &ivlRight, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_MUL:
                IA_EvalPop(ivlRight);
                IA_EvalPop(ivlLeft);
                IA_Multiply(&ivlLeft, &ivlRight, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_DIV:
                IA_EvalPop(ivlRight);
                IA_EvalPop(ivlLeft);
                IA_Divide(&ivlLeft, &ivlRight, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_POW:
                IA_EvalPop(ivlRight);
                IA_EvalPop(ivlLeft);
                IA_Power(&ivlLeft, &ivlRight, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_INTDIV:
                IA_EvalPop(ivlRight);
                IA_EvalPop(ivlLeft);
                IA_IntegerDivide(&ivlLeft, &ivlRight, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_MOD:
                IA_EvalPop(ivlRight);
                IA_EvalPop(ivlLeft);
                IA_Modulo(&ivlLeft, &ivlRight, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_NOT:
                IA_EvalPop(ivlLeft);
                IA_Not(&ivlLeft, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_AND:
                IA_EvalPop(ivlRight);
                IA_EvalPop(ivlLeft);
                IA_And(&ivlLeft, &ivlRight, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_OR:
                IA_EvalPop(ivlRight);
                IA_EvalPop(ivlLeft);
                IA_Or(&ivlLeft, &ivlRight, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_EQUAL:
                IA_EvalPop(ivlRight);
                IA_EvalPop(ivlLeft);
                IA_Equal(&ivlLeft, &ivlRight, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_NEQ:
                IA_EvalPop(ivlRight);
                IA_EvalPop(ivlLeft);
                IA_NotEqual(&ivlLeft, &ivlRight, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_GT:
                IA_EvalPop(ivlRight);
                IA_EvalPop(ivlLeft);
                IA_GreaterThan(&ivlLeft, &ivlRight, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_LT:
                IA_EvalPop(ivlRight);
                IA_EvalPop(ivlLeft);
                IA_LessThan(&ivlLeft, &ivlRight, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_GTEQ:
                IA_EvalPop(ivlRight);
                IA_EvalPop(ivlLeft);
                IA_GreaterOrEqualThan(&ivlLeft, &ivlRight, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_LTEQ:
                IA_EvalPop(ivlRight);
                IA_EvalPop(ivlLeft);
                IA_LessOrEqualThan(&ivlLeft, &ivlRight, &ivlResult);
                IA_EvalPush(ivlResult);
                break;
            case EZOP_PUSH_IMD:
                ivlResult.fLow = pInst->uData.fImmediate;
                ivlResult.fHigh =pInst->uData.fImmediate;
                IA_EvalPush(ivlResult);
                break;
            case EZOP_PUSH_VAR:
                IA_EvalPush(pIaVm->pVariableValueArray[pInst->uData.iVarIndex]);
                break;
            case EZOP_FUNC:
                EzAiMachine_Function(pIaVm, pInst->uData.iFuncIndex);
                break;
        }
    }

    /* Workaround: Apply negation here to ensure correct image rendering. */
    /* IntervalSet(*pIvlOut, pIaVm->pStack[0]); */
    IA_Not(&pIaVm->pStack[0], pIvlOut);
    
    return pIvlOut;
}

/* Basic arithmetic operations */
EzInterval* IA_Negative(EzInterval* pIvlLeft, EzInterval* pIvlOut) {
    /* [-X_high, -X_low] */
    pIvlOut->fLow = -pIvlLeft->fHigh;
    pIvlOut->fHigh = -pIvlLeft->fLow;
    return pIvlOut;
}

EzInterval* IA_Add(EzInterval* pIvlLeft, EzInterval* pIvlRight, EzInterval* pIvlOut) {
    pIvlOut->fLow = pIvlLeft->fLow + pIvlRight->fLow;
    pIvlOut->fHigh = pIvlLeft->fHigh + pIvlRight->fHigh;
    return pIvlOut;
}

EzInterval* IA_Subtract(EzInterval* pIvlLeft, EzInterval* pIvlRight, EzInterval* pIvlOut) {
    pIvlOut->fLow = pIvlLeft->fLow - pIvlRight->fHigh;
    pIvlOut->fHigh = pIvlLeft->fHigh - pIvlRight->fLow;
    return pIvlOut;
}

EzInterval* IA_Multiply(EzInterval* pIvlLeft, EzInterval* pIvlRight, EzInterval* pIvlOut) {
    PZ_FLOAT p1 = pIvlLeft->fLow * pIvlRight->fLow;
    PZ_FLOAT p2 = pIvlLeft->fLow * pIvlRight->fHigh;
    PZ_FLOAT p3 = pIvlLeft->fHigh * pIvlRight->fLow;
    PZ_FLOAT p4 = pIvlLeft->fHigh * pIvlRight->fHigh;

    pIvlOut->fLow = IA_MIN4(p1, p2, p3, p4);
    pIvlOut->fHigh = IA_MAX4(p1, p2, p3, p4);
    return pIvlOut;
}

EzInterval* IA_Divide(EzInterval* pIvlLeft, EzInterval* pIvlRight, EzInterval* pIvlOut) {
    /* Check if denominator crosses zero */
    if (pIvlRight->fLow <= 0.0 && pIvlRight->fHigh >= 0.0) {
        /* If denominator contains 0, result diverges to infinity */
        pIvlOut->fLow = -PZ_INFINITY;
        pIvlOut->fHigh = PZ_INFINITY;
    } else {
        PZ_FLOAT p1 = pIvlLeft->fLow / pIvlRight->fLow;
        PZ_FLOAT p2 = pIvlLeft->fLow / pIvlRight->fHigh;
        PZ_FLOAT p3 = pIvlLeft->fHigh / pIvlRight->fLow;
        PZ_FLOAT p4 = pIvlLeft->fHigh / pIvlRight->fHigh;
        
        pIvlOut->fLow = IA_MIN4(p1, p2, p3, p4);
        pIvlOut->fHigh = IA_MAX4(p1, p2, p3, p4);
    }
    return pIvlOut;
}

/* Advanced arithmetic and discrete operations */
EzInterval* IA_Power(EzInterval* pIvlLeft, EzInterval* pIvlRight, EzInterval* pIvlOut) {
    /* Simplified: for constant positive integer exponent (e.g. X^2, X^3), apply extremum logic */
    if (pIvlRight->fLow == pIvlRight->fHigh && floor(pIvlRight->fLow) == pIvlRight->fLow) {
        int exp = (int)pIvlRight->fLow;
        if (exp % 2 == 0) {
            /* Even exponent, interval crosses 0, lower bound is 0 */
            if (pIvlLeft->fLow <= 0.0 && pIvlLeft->fHigh >= 0.0) {
                pIvlOut->fLow = 0.0;
                pIvlOut->fHigh = IA_MAX(pow(pIvlLeft->fLow, exp), pow(pIvlLeft->fHigh, exp));
                return pIvlOut;
            }
        }
    }
    
    /* General case, may produce very wide bounds */
    {
        PZ_FLOAT p1 = pow(pIvlLeft->fLow, pIvlRight->fLow);
        PZ_FLOAT p2 = pow(pIvlLeft->fLow, pIvlRight->fHigh);
        PZ_FLOAT p3 = pow(pIvlLeft->fHigh, pIvlRight->fLow);
        PZ_FLOAT p4 = pow(pIvlLeft->fHigh, pIvlRight->fHigh);
        pIvlOut->fLow = IA_MIN4(p1, p2, p3, p4);
        pIvlOut->fHigh = IA_MAX4(p1, p2, p3, p4);
    }
    return pIvlOut;
}

EzInterval* IA_IntegerDivide(EzInterval* pIvlLeft, EzInterval* pIvlRight, EzInterval* pIvlOut) {
    EzInterval divResult;
    IA_Divide(pIvlLeft, pIvlRight, &divResult);
    pIvlOut->fLow = floor(divResult.fLow);
    pIvlOut->fHigh = floor(divResult.fHigh);
    return pIvlOut;
}

EzInterval* IA_Modulo(EzInterval* pIvlLeft, EzInterval* pIvlRight, EzInterval* pIvlOut) {
    /* Strictly conservative modulo interval: [0, |max divisor|] */
    PZ_FLOAT maxMod = IA_MAX(fabs(pIvlRight->fLow), fabs(pIvlRight->fHigh));
    pIvlOut->fLow = 0.0;
    pIvlOut->fHigh = maxMod;
    return pIvlOut;
}

/* Logic and comparison operations */
EzInterval* IA_Not(EzInterval* pIvlLeft, EzInterval* pIvlOut) {
    /* Assume 0=false, 1=true. Not(X) = 1 - X */
    pIvlOut->fLow = 1.0 - pIvlLeft->fHigh;
    pIvlOut->fHigh = 1.0 - pIvlLeft->fLow;
    /* Clamp to [0, 1] */
    pIvlOut->fLow = IA_MAX(0.0, pIvlOut->fLow);
    pIvlOut->fHigh = IA_MIN(1.0, pIvlOut->fHigh);
    return pIvlOut;
}

EzInterval* IA_And(EzInterval* pIvlLeft, EzInterval* pIvlRight, EzInterval* pIvlOut) {
    pIvlOut->fLow = IA_MIN(pIvlLeft->fLow, pIvlRight->fLow);
    pIvlOut->fHigh = IA_MIN(pIvlLeft->fHigh, pIvlRight->fHigh);
    return pIvlOut;
}

EzInterval* IA_Or(EzInterval* pIvlLeft, EzInterval* pIvlRight, EzInterval* pIvlOut) {
    pIvlOut->fLow = IA_MAX(pIvlLeft->fLow, pIvlRight->fLow);
    pIvlOut->fHigh = IA_MAX(pIvlLeft->fHigh, pIvlRight->fHigh);
    return pIvlOut;
}

EzInterval* IA_Equal(EzInterval* pIvlLeft, EzInterval* pIvlRight, EzInterval* pIvlOut) {
    if (pIvlLeft->fHigh < pIvlRight->fLow || pIvlLeft->fLow > pIvlRight->fHigh) {
        pIvlOut->fLow = 0.0; pIvlOut->fHigh = 0.0; /* Definitely false */
    } else if (pIvlLeft->fLow == pIvlRight->fLow && pIvlLeft->fHigh == pIvlRight->fHigh) {
        pIvlOut->fLow = 1.0; pIvlOut->fHigh = 1.0; /* Same range, definitely equal */
    } else {
        PZ_FLOAT rangeL = pIvlLeft->fHigh - pIvlLeft->fLow;
        PZ_FLOAT rangeR = pIvlRight->fHigh - pIvlRight->fLow;
        PZ_FLOAT dist = pIvlLeft->fLow - pIvlRight->fLow;
        if (dist < 0.0f) dist = -dist;
        if (rangeL < 1e-12f && rangeR < 1e-12f && dist < 1e-12f) {
            pIvlOut->fLow = 1.0; pIvlOut->fHigh = 1.0; /* Degenerate equal points */
        } else {
            pIvlOut->fLow = 0.0; pIvlOut->fHigh = 1.0; /* Possibly true or false */
        }
    }
    return pIvlOut;
}

EzInterval* IA_NotEqual(EzInterval* pIvlLeft, EzInterval* pIvlRight, EzInterval* pIvlOut) {
    EzInterval eqOut;
    IA_Equal(pIvlLeft, pIvlRight, &eqOut);
    return IA_Not(&eqOut, pIvlOut);
}

EzInterval* IA_LessThan(EzInterval* pIvlLeft, EzInterval* pIvlRight, EzInterval* pIvlOut) {
    if (pIvlLeft->fHigh < pIvlRight->fLow) {
        pIvlOut->fLow = 1.0; pIvlOut->fHigh = 1.0;
    } else if (pIvlLeft->fLow >= pIvlRight->fHigh) {
        pIvlOut->fLow = 0.0; pIvlOut->fHigh = 0.0;
    } else {
        pIvlOut->fLow = 0.0; pIvlOut->fHigh = 1.0;
    }
    return pIvlOut;
}

EzInterval* IA_GreaterThan(EzInterval* pIvlLeft, EzInterval* pIvlRight, EzInterval* pIvlOut) {
    return IA_LessThan(pIvlRight, pIvlLeft, pIvlOut);
}

EzInterval* IA_LessOrEqualThan(EzInterval* pIvlLeft, EzInterval* pIvlRight, EzInterval* pIvlOut) {
    if (pIvlLeft->fHigh <= pIvlRight->fLow) {
        pIvlOut->fLow = 1.0; pIvlOut->fHigh = 1.0;
    } else if (pIvlLeft->fLow > pIvlRight->fHigh) {
        pIvlOut->fLow = 0.0; pIvlOut->fHigh = 0.0;
    } else {
        pIvlOut->fLow = 0.0; pIvlOut->fHigh = 1.0;
    }
    return pIvlOut;
}

EzInterval* IA_GreaterOrEqualThan(EzInterval* pIvlLeft, EzInterval* pIvlRight, EzInterval* pIvlOut) {
    return IA_LessOrEqualThan(pIvlRight, pIvlLeft, pIvlOut);
}

/* Math functions: non-monotonic and monotonic */
EzInterval* IA_Absolute(EzInterval* pIvlArg, EzInterval* pIvlOut) {
    if (pIvlArg->fLow <= 0.0 && pIvlArg->fHigh >= 0.0) {
        pIvlOut->fLow = 0.0;
        pIvlOut->fHigh = IA_MAX(fabs(pIvlArg->fLow), fabs(pIvlArg->fHigh));
    } else {
        PZ_FLOAT a = fabs(pIvlArg->fLow);
        PZ_FLOAT b = fabs(pIvlArg->fHigh);
        pIvlOut->fLow = IA_MIN(a, b);
        pIvlOut->fHigh = IA_MAX(a, b);
    }
    return pIvlOut;
}

EzInterval* IA_SquareRoot(EzInterval* pIvlArg, EzInterval* pIvlOut) {
    /* Prevent NaN crash from sqrt of negative numbers */
    PZ_FLOAT safeLow = IA_MAX(0.0, pIvlArg->fLow);
    PZ_FLOAT safeHigh = IA_MAX(0.0, pIvlArg->fHigh);
    pIvlOut->fLow = sqrt(safeLow);
    pIvlOut->fHigh = sqrt(safeHigh);
    return pIvlOut;
}

EzInterval* IA_Log(EzInterval* pIvlArg, EzInterval* pIvlOut) {
    /* Prevent domain error */
    PZ_FLOAT safeLow = IA_MAX(1e-15, pIvlArg->fLow);
    PZ_FLOAT safeHigh = IA_MAX(1e-15, pIvlArg->fHigh);
    pIvlOut->fLow = log(safeLow);
    pIvlOut->fHigh = log(safeHigh);
    return pIvlOut;
}

EzInterval* IA_NaturalExponential(EzInterval* pIvlArg, EzInterval* pIvlOut) {
    pIvlOut->fLow = exp(pIvlArg->fLow);
    pIvlOut->fHigh = exp(pIvlArg->fHigh);
    return pIvlOut;
}

EzInterval* IA_Sine(EzInterval* pIvlArg, EzInterval* pIvlOut) {
    if (pIvlArg->fHigh - pIvlArg->fLow >= 2.0 * PZ_PI) {
        pIvlOut->fLow = -1.0;
        pIvlOut->fHigh = 1.0;
    } else {
        PZ_FLOAT v1 = sin(pIvlArg->fLow);
        PZ_FLOAT v2 = sin(pIvlArg->fHigh);
        PZ_FLOAT min_val = IA_MIN(v1, v2);
        PZ_FLOAT max_val = IA_MAX(v1, v2);
        
        PZ_FLOAT peak_low, peak_high, valley_low, valley_high;
        
        peak_low  = ceil((pIvlArg->fLow - PZ_PI / 2.0) / (2.0 * PZ_PI));
        peak_high = floor((pIvlArg->fHigh - PZ_PI / 2.0) / (2.0 * PZ_PI));
        
        if (peak_low <= peak_high) {
            max_val = 1.0; /* Peak */
        }
        
        valley_low  = ceil((pIvlArg->fLow + PZ_PI / 2.0) / (2.0 * PZ_PI));
        valley_high = floor((pIvlArg->fHigh + PZ_PI / 2.0) / (2.0 * PZ_PI));
        
        if (valley_low <= valley_high) {
            min_val = -1.0; /* Valley */
        }
        
        pIvlOut->fLow = min_val;
        pIvlOut->fHigh = max_val;
    }
    return pIvlOut;
}

EzInterval* IA_Cosine(EzInterval* pIvlArg, EzInterval* pIvlOut) {
    EzInterval shifted;
    shifted.fLow = pIvlArg->fLow + PZ_PI / 2.0;
    shifted.fHigh = pIvlArg->fHigh + PZ_PI / 2.0;
    return IA_Sine(&shifted, pIvlOut);
}

EzInterval* IA_Tangent(EzInterval* pIvlArg, EzInterval* pIvlOut) {
    PZ_FLOAT k_low = floor((pIvlArg->fLow - PZ_PI / 2.0) / PZ_PI);
    PZ_FLOAT k_high = floor((pIvlArg->fHigh - PZ_PI / 2.0) / PZ_PI);
    
    if (k_low != k_high) {
        /* Crossed an asymptote */
        pIvlOut->fLow = -PZ_INFINITY;
        pIvlOut->fHigh = PZ_INFINITY;
    } else {
        pIvlOut->fLow = tan(pIvlArg->fLow);
        pIvlOut->fHigh = tan(pIvlArg->fHigh);
    }
    return pIvlOut;
}

EzInterval* IA_Arcsine(EzInterval* pIvlArg, EzInterval* pIvlOut) {
    PZ_FLOAT safeLow = IA_MAX(-1.0, IA_MIN(1.0, pIvlArg->fLow));
    PZ_FLOAT safeHigh = IA_MAX(-1.0, IA_MIN(1.0, pIvlArg->fHigh));
    pIvlOut->fLow = asin(safeLow);
    pIvlOut->fHigh = asin(safeHigh);
    return pIvlOut;
}

EzInterval* IA_Arccosine(EzInterval* pIvlArg, EzInterval* pIvlOut) {
    /* Acos is decreasing, endpoints need swapping */
    PZ_FLOAT safeLow = IA_MAX(-1.0, IA_MIN(1.0, pIvlArg->fLow));
    PZ_FLOAT safeHigh = IA_MAX(-1.0, IA_MIN(1.0, pIvlArg->fHigh));
    pIvlOut->fLow = acos(safeHigh);
    pIvlOut->fHigh = acos(safeLow);
    return pIvlOut;
}

EzInterval* IA_Arctangent(EzInterval* pIvlArg, EzInterval* pIvlOut) {
    pIvlOut->fLow = atan(pIvlArg->fLow);
    pIvlOut->fHigh = atan(pIvlArg->fHigh);
    return pIvlOut;
}