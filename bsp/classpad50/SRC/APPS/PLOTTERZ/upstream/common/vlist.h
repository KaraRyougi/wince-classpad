#ifndef _V_LIST_H_
#define _V_LIST_H_

typedef struct tagVlistNode {
    struct tagVlistNode *pPrev, *pNext;
    void *pData;
} VlistNode;

typedef struct {
    VlistNode *pHead, *pTail;
    int iSize;
} Vlist;

Vlist*      vlNewList       ();
Vlist*      vlPushBack      (Vlist* pList, void *pData);
Vlist*      vlPushFront     (Vlist* pList, void *pData);
void*       vlPopFront      (Vlist* pList);
void*       vlPopBack       (Vlist* pList);
void        vlDestroy       (Vlist* pList, void (* releaseData)(void *));

#define vlPeek(pList)       ((pList)->pTail->pData)

typedef Vlist VQueue;
#define vqNewQueue                  vlNewList
#define vqPush                      vlPushBack
#define vqPop                       vlPopFront
#define vqIsEmpty(q)                ((q)->iSize <= 0)
#define vqDestroy(q, releaseData)   vlDestroy(q, releaseData)

#endif