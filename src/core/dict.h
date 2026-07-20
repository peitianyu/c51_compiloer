#ifndef TTCC_DICT_H
#define TTCC_DICT_H

#include <stdlib.h>
#include <string.h>
#include "list.h"

typedef struct Dict {
    List *list;
    struct Dict *parent;
} Dict;

#define EMPTY_DICT ((Dict){&EMPTY_LIST, NULL})

typedef struct {
    char *key;
    void *val;
} DictEntry;

static inline void *make_dict(void *parent)
{
    Dict *r = malloc(sizeof(Dict));
    r->list = make_list();
    r->parent = parent;
    return r;
}

static inline void *dict_get(Dict *dict, const char *key)
{
    for (; dict; dict = dict->parent) {
        void *found = NULL;
        for (Iter i = list_iter(dict->list); !iter_end(i);) {
            DictEntry *e = iter_next(&i);
            if (!strcmp(key, e->key)) {
                /* 语义：同一作用域后写覆盖先写，返回最后一个匹配项 */
                found = e->val;
            }
        }
        if (found) return found;
    }
    return NULL;
}

static inline void dict_put(Dict *dict, char *key, void *val)
{
    DictEntry *e = malloc(sizeof(DictEntry));
    e->key = key;
    e->val = val;
    list_push(dict->list, e);
}

static inline bool dict_remove(Dict *dict, char *key)
{
    if (!dict) return false;
    
    ListNode *prev = NULL;
    ListNode *node = dict->list->head;
    
    while (node) {
        DictEntry *entry = (DictEntry *)node->elem;
        if (!strcmp(entry->key, key)) {
            if (prev) {
                prev->next = node->next;
                if (node->next) {
                    node->next->prev = prev;
                } else {
                    dict->list->tail = prev;
                }
            } else {
                dict->list->head = node->next;
                if (node->next) {
                    node->next->prev = NULL;
                } else {
                    dict->list->tail = NULL;
                }
            }
            /* 注意：不 free entry->key —— 字符串可能由 Arena 管理，
             * 也可能由调用方管理。调用方负责 key 生命周期。 */
            free(entry);
            free(node);
            dict->list->len--;
            return true;
        }
        
        prev = node;
        node = node->next;
    }
    
    return false;
}

static inline List *dict_keys(Dict *dict)
{
    List *r = make_list();
    for (; dict; dict = dict->parent)
        for (Iter i = list_iter(dict->list); !iter_end(i);)
            list_push(r, ((DictEntry *) iter_next(&i))->key);
    return r;
}

static inline List *dict_values(Dict *dict)
{
    List *r = make_list();
    for (; dict; dict = dict->parent)
        for (Iter i = list_iter(dict->list); !iter_end(i);)
            list_push(r, ((DictEntry *) iter_next(&i))->val);
    return r;
}

static inline void dict_clear(Dict *dict)
{
    list_free(dict->list);
    free(dict->list);
    free(dict);
}

/* 清空 dict 内容（entries），保留 dict 结构本身以便重用 */
static inline void dict_reset(Dict *dict)
{
    if (!dict) return;
    list_clear(dict->list);
}

static inline void dict_free(Dict *dict, void (*free_val)(void*))
{
    if (!dict) return;
    
    // 遍历并释放每个 entry
    ListNode *node = dict->list->head;
    while (node) {
        ListNode *next = node->next;
        DictEntry *entry = (DictEntry *)node->elem;
        free(entry->key);
        if (free_val) free_val(entry->val);
        free(entry);
        free(node);
        node = next;
    }
    
    // 释放 list 结构本身
    free(dict->list);
    free(dict);
}

static inline void *dict_parent(Dict *dict)
{
    void *r = dict->parent;
    list_free(dict->list);
    free(dict->list);
    free(dict);
    return r;
}

#endif /* TTCC_DICT_H */
