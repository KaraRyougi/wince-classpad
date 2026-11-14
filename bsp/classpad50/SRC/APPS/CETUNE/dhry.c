//------------------------------------------------------------------------------
//  dhry.c - Dhrystone 2.1 (Reinhold P. Weicker), the integer benchmark of
//  Xtune's "INT: n Dhrystone/s" line.
//
//  Xtune takes it from the cBench library (cBench/dhry.h, dhrystone(runs)),
//  whose source was not to be had when this was written; this is the
//  published 2.1 program, the records static instead of malloc'd, in one
//  file. dhrystone() runs the measured loop `runs` times and then checks the
//  values the program is documented to end with: 1 when they are right. A
//  clock setting that makes the CPU or the memory miscalculate shows here.
//------------------------------------------------------------------------------
#include <string.h>

typedef enum { Ident_1, Ident_2, Ident_3, Ident_4, Ident_5 } Enumeration;
typedef int  One_Thirty;
typedef int  One_Fifty;
typedef char Capital_Letter;
typedef int  Boolean;
typedef char Str_30[31];
typedef int  Arr_1_Dim[50];
typedef int  Arr_2_Dim[50][50];

typedef struct record {
    struct record *Ptr_Comp;
    Enumeration    Discr;
    union {
        struct { Enumeration Enum_Comp; int Int_Comp; char Str_Comp[31]; } var_1;
        struct { Enumeration E_Comp_2; char Str_2_Comp[31]; } var_2;
        struct { char Ch_1_Comp; char Ch_2_Comp; } var_3;
    } variant;
} Rec_Type, *Rec_Pointer;

static Rec_Pointer Ptr_Glob, Next_Ptr_Glob;
static int         Int_Glob;
static Boolean     Bool_Glob;
static char        Ch_1_Glob, Ch_2_Glob;
static int         Arr_1_Glob[50];
static int         Arr_2_Glob[50][50];
static Rec_Type    Rec_1, Rec_2;

static void Proc_7(One_Fifty Int_1_Par_Val, One_Fifty Int_2_Par_Val, One_Fifty *Int_Par_Ref)
{
    One_Fifty Int_Loc;
    Int_Loc = Int_1_Par_Val + 2;
    *Int_Par_Ref = Int_2_Par_Val + Int_Loc;
}

static void Proc_8(Arr_1_Dim Arr_1_Par_Ref, Arr_2_Dim Arr_2_Par_Ref, int Int_1_Par_Val, int Int_2_Par_Val)
{
    One_Fifty Int_Index, Int_Loc;
    Int_Loc = Int_1_Par_Val + 5;
    Arr_1_Par_Ref[Int_Loc] = Int_2_Par_Val;
    Arr_1_Par_Ref[Int_Loc + 1] = Arr_1_Par_Ref[Int_Loc];
    Arr_1_Par_Ref[Int_Loc + 30] = Int_Loc;
    for (Int_Index = Int_Loc; Int_Index <= Int_Loc + 1; ++Int_Index)
        Arr_2_Par_Ref[Int_Loc][Int_Index] = Int_Loc;
    Arr_2_Par_Ref[Int_Loc][Int_Loc - 1] += 1;
    Arr_2_Par_Ref[Int_Loc + 20][Int_Loc] = Arr_1_Par_Ref[Int_Loc];
    Int_Glob = 5;
}

static Enumeration Func_1(Capital_Letter Ch_1_Par_Val, Capital_Letter Ch_2_Par_Val)
{
    Capital_Letter Ch_1_Loc, Ch_2_Loc;
    Ch_1_Loc = Ch_1_Par_Val;
    Ch_2_Loc = Ch_1_Loc;
    if (Ch_2_Loc != Ch_2_Par_Val) return Ident_1;
    Ch_1_Glob = Ch_1_Loc;
    return Ident_2;
}

static Boolean Func_2(Str_30 Str_1_Par_Ref, Str_30 Str_2_Par_Ref)
{
    One_Thirty     Int_Loc;
    Capital_Letter Ch_Loc = 0;
    Int_Loc = 2;
    while (Int_Loc <= 2)
        if (Func_1(Str_1_Par_Ref[Int_Loc], Str_2_Par_Ref[Int_Loc + 1]) == Ident_1) {
            Ch_Loc = 'A';
            Int_Loc += 1;
        }
    if (Ch_Loc >= 'W' && Ch_Loc < 'Z') Int_Loc = 7;
    if (Ch_Loc == 'R') return 1;
    if (strcmp(Str_1_Par_Ref, Str_2_Par_Ref) > 0) {
        Int_Loc += 7;
        Int_Glob = Int_Loc;
        return 1;
    }
    return 0;
}

static Boolean Func_3(Enumeration Enum_Par_Val)
{
    Enumeration Enum_Loc;
    Enum_Loc = Enum_Par_Val;
    if (Enum_Loc == Ident_3) return 1;
    return 0;
}

static void Proc_6(Enumeration Enum_Val_Par, Enumeration *Enum_Ref_Par)
{
    *Enum_Ref_Par = Enum_Val_Par;
    if (!Func_3(Enum_Val_Par)) *Enum_Ref_Par = Ident_4;
    switch (Enum_Val_Par) {
    case Ident_1: *Enum_Ref_Par = Ident_1; break;
    case Ident_2: if (Int_Glob > 100) *Enum_Ref_Par = Ident_1; else *Enum_Ref_Par = Ident_4; break;
    case Ident_3: *Enum_Ref_Par = Ident_2; break;
    case Ident_4: break;
    case Ident_5: *Enum_Ref_Par = Ident_3; break;
    }
}

static void Proc_3(Rec_Pointer *Ptr_Ref_Par)
{
    if (Ptr_Glob != 0) *Ptr_Ref_Par = Ptr_Glob->Ptr_Comp;
    Proc_7(10, Int_Glob, &Ptr_Glob->variant.var_1.Int_Comp);
}

static void Proc_1(Rec_Pointer Ptr_Val_Par)
{
    Rec_Pointer Next_Record = Ptr_Val_Par->Ptr_Comp;
    *Ptr_Val_Par->Ptr_Comp = *Ptr_Glob;
    Ptr_Val_Par->variant.var_1.Int_Comp = 5;
    Next_Record->variant.var_1.Int_Comp = Ptr_Val_Par->variant.var_1.Int_Comp;
    Next_Record->Ptr_Comp = Ptr_Val_Par->Ptr_Comp;
    Proc_3(&Next_Record->Ptr_Comp);
    if (Next_Record->Discr == Ident_1) {
        Next_Record->variant.var_1.Int_Comp = 6;
        Proc_6(Ptr_Val_Par->variant.var_1.Enum_Comp, &Next_Record->variant.var_1.Enum_Comp);
        Next_Record->Ptr_Comp = Ptr_Glob->Ptr_Comp;
        Proc_7(Next_Record->variant.var_1.Int_Comp, 10, &Next_Record->variant.var_1.Int_Comp);
    } else {
        *Ptr_Val_Par = *Ptr_Val_Par->Ptr_Comp;
    }
}

static void Proc_2(One_Fifty *Int_Par_Ref)
{
    One_Fifty   Int_Loc;
    Enumeration Enum_Loc = Ident_2;
    Int_Loc = *Int_Par_Ref + 10;
    do {
        if (Ch_1_Glob == 'A') {
            Int_Loc -= 1;
            *Int_Par_Ref = Int_Loc - Int_Glob;
            Enum_Loc = Ident_1;
        }
    } while (Enum_Loc != Ident_1);
}

static void Proc_4(void)
{
    Boolean Bool_Loc;
    Bool_Loc = Ch_1_Glob == 'A';
    Bool_Glob = Bool_Loc | Bool_Glob;
    Ch_2_Glob = 'B';
}

static void Proc_5(void)
{
    Ch_1_Glob = 'A';
    Bool_Glob = 0;
}

int dhrystone(int Number_Of_Runs)
{
    One_Fifty   Int_1_Loc = 0, Int_2_Loc = 0, Int_3_Loc = 0;
    char        Ch_Index;
    Enumeration Enum_Loc = Ident_1;
    Str_30      Str_1_Loc, Str_2_Loc;
    int         Run_Index;

    Next_Ptr_Glob = &Rec_1;
    Ptr_Glob = &Rec_2;
    memset(&Rec_1, 0, sizeof Rec_1);
    memset(Arr_1_Glob, 0, sizeof Arr_1_Glob);
    memset(Arr_2_Glob, 0, sizeof Arr_2_Glob);
    Int_Glob = 0; Bool_Glob = 0; Ch_1_Glob = 0; Ch_2_Glob = 0;

    Ptr_Glob->Ptr_Comp = Next_Ptr_Glob;
    Ptr_Glob->Discr = Ident_1;
    Ptr_Glob->variant.var_1.Enum_Comp = Ident_3;
    Ptr_Glob->variant.var_1.Int_Comp = 40;
    strcpy(Ptr_Glob->variant.var_1.Str_Comp, "DHRYSTONE PROGRAM, SOME STRING");
    strcpy(Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING");
    Arr_2_Glob[8][7] = 10;

    for (Run_Index = 1; Run_Index <= Number_Of_Runs; ++Run_Index) {
        Proc_5();
        Proc_4();
        Int_1_Loc = 2;
        Int_2_Loc = 3;
        strcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING");
        Enum_Loc = Ident_2;
        Bool_Glob = !Func_2(Str_1_Loc, Str_2_Loc);
        while (Int_1_Loc < Int_2_Loc) {
            Int_3_Loc = 5 * Int_1_Loc - Int_2_Loc;
            Proc_7(Int_1_Loc, Int_2_Loc, &Int_3_Loc);
            Int_1_Loc += 1;
        }
        Proc_8(Arr_1_Glob, Arr_2_Glob, Int_1_Loc, Int_3_Loc);
        Proc_1(Ptr_Glob);
        for (Ch_Index = 'A'; Ch_Index <= Ch_2_Glob; ++Ch_Index) {
            if (Enum_Loc == Func_1(Ch_Index, 'C')) {
                Proc_6(Ident_1, &Enum_Loc);
                strcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 3'RD STRING");
                Int_2_Loc = Run_Index;
                Int_Glob = Run_Index;
            }
        }
        Int_2_Loc = Int_2_Loc * Int_1_Loc;
        Int_1_Loc = Int_2_Loc / Int_3_Loc;
        Int_2_Loc = 7 * (Int_2_Loc - Int_3_Loc) - Int_1_Loc;
        Proc_2(&Int_1_Loc);
    }

    // the values the program's own listing says it must end with
    return Int_Glob == 5 && Bool_Glob == 1 && Ch_1_Glob == 'A' && Ch_2_Glob == 'B'
        && Arr_1_Glob[8] == 7 && Arr_2_Glob[8][7] == Number_Of_Runs + 10
        && Ptr_Glob->Ptr_Comp == Next_Ptr_Glob && Ptr_Glob->Discr == Ident_1
        && Ptr_Glob->variant.var_1.Enum_Comp == Ident_3 && Ptr_Glob->variant.var_1.Int_Comp == 17
        && !strcmp(Ptr_Glob->variant.var_1.Str_Comp, "DHRYSTONE PROGRAM, SOME STRING")
        && Next_Ptr_Glob->Ptr_Comp == Next_Ptr_Glob && Next_Ptr_Glob->Discr == Ident_1
        && Next_Ptr_Glob->variant.var_1.Enum_Comp == Ident_2 && Next_Ptr_Glob->variant.var_1.Int_Comp == 18
        && !strcmp(Next_Ptr_Glob->variant.var_1.Str_Comp, "DHRYSTONE PROGRAM, SOME STRING")
        && Int_1_Loc == 5 && Int_2_Loc == 13 && Int_3_Loc == 7 && Enum_Loc == Ident_2
        && !strcmp(Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING")
        && !strcmp(Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING");
}
