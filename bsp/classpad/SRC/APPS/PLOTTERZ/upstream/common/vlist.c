#include <stdlib.h>
#include "vlist.h"

static VlistNode* vlnNewNode(void *pData) {
    VlistNode* n = (VlistNode *)malloc(sizeof(VlistNode));
    n->pPrev = n->pNext = NULL;
    n->pData = pData;
    return n;
}

Vlist* vlNewList() {
    Vlist *l = (Vlist *)malloc(sizeof(Vlist));
    l->pHead = l->pTail = NULL;
    l->iSize = 0;
    return l;
}

Vlist* vlPushBack(Vlist* pList, void *pData) {
    VlistNode *newNode = vlnNewNode(pData);

    if (pList->pHead == NULL) {
        pList->pHead = pList->pTail = newNode;
    }
    else {
        VlistNode * pTail = pList->pTail;
        pTail->pNext = newNode;
        newNode->pPrev = pTail;
        pList->pTail = newNode;
    }

    pList->iSize++;

    return pList;
}

Vlist* vlPushFront(Vlist* pList, void *pData) {
    VlistNode *newNode = vlnNewNode(pData);

    if (pList->pHead == NULL) {
        pList->pHead = pList->pTail = newNode;
    }
    else {
        VlistNode * pHead = pList->pHead;
        pHead->pPrev = newNode;
        newNode->pNext = pHead;
        pList->pHead = newNode;
    }

    pList->iSize++;

    return pList;
}

void* vlPopFront(Vlist* pList) {
    VlistNode *pHead;
    void *pData;

    pHead = pList->pHead;

    if (pHead == NULL) return NULL;

    pData = pHead->pData;

    pList->pHead = pHead->pNext;
    if (pList->pHead) {
        pList->pHead->pPrev = NULL;
    }
    else {
        pList->pTail = NULL;
    }

    free(pHead);
    pList->iSize--;

    return pData;
}

void* vlPopBack(Vlist* pList) {
    VlistNode *pTail, *prevTail; 
    void * pData;

    if (pList->iSize <= 0) {
        return NULL;
    }

    pTail = pList->pTail;
    pData = pTail->pData;

    if (pList->iSize == 1) {
        pList->pHead = NULL;
        pList->pTail = NULL;
        free(pTail);
        pList->iSize = 0;
        return pData;
    }

    prevTail = pTail->pPrev;
    prevTail->pNext = NULL;
    pList->pTail = prevTail;
    free(pTail);
    pList->iSize--;

    return pData;
}

static void vlnDestroy(VlistNode* vln, void (* releaseData)(void *)) {
    if (vln->pData != NULL && releaseData) {
        releaseData(vln->pData);
        vln->pData = NULL;
    }
    free(vln);
}

void vlDestroy(Vlist* pList, void (* releaseData)(void *)) {
    VlistNode *n1, *n2;

    n1 = pList->pHead;
    while (n1) {
        n2 = n1->pNext;
        vlnDestroy(n1, releaseData);
        n1 = n2;
    }

    free(pList);
}