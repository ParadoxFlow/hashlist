#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h>

typedef struct {
    Py_ssize_t *indices;
    Py_ssize_t size;
    Py_ssize_t capacity;
} BucketVector;

typedef struct {
    PyObject_HEAD
    PyObject **ob_item;      
    Py_ssize_t allocated;    
    Py_ssize_t size;         
    
    BucketVector *buckets;
    Py_ssize_t *bucket_tokens; 
    Py_ssize_t table_capacity;  
    int is_dirty;              
} HashListObject;

// Быстрая и безопасная Си-проверка жесткого равенства с учетом типов True/1 и False/0
static inline int fast_rich_compare_strict(PyObject *s, PyObject *i) {
    if (s == i) return 1;
    // Если один из объектов bool, а второй нет — они строго НЕ равны (убираем коллизию True == 1)
    if (PyBool_Check(s) != PyBool_Check(i)) return 0;
    return PyObject_RichCompareBool(s, i, Py_EQ) == 1;
}

// Детерминированный хэш по значению без сортировок ключей и генерации строк
static Py_ssize_t get_safe_token_c(PyObject *item) {
    if (item == NULL) return 0;
    
    // Для булевых значений даем уникальные изолированные токены, чтобы развести их с 1 и 0
    if (PyBool_Check(item)) {
        return item == Py_True ? 999999991 : 999999990;
    }
    
    Py_ssize_t h = PyObject_Hash(item);
    if (h != -1) return h;
    PyErr_Clear(); 
    
    // 1. ДЛЯ СЛОВАРЕЙ: Суммируем хэши пар
    if (PyDict_Check(item)) {
        Py_ssize_t dict_hash = PyDict_GET_SIZE(item);
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(item, &pos, &key, &value)) {
            Py_ssize_t k_hash = PyObject_Hash(key);
            if (k_hash == -1) PyErr_Clear();
            Py_ssize_t v_hash = PyObject_Hash(value);
            if (v_hash == -1) PyErr_Clear();
            dict_hash += (k_hash ^ v_hash);
        }
        return dict_hash;
    }
    
    // 2. ДЛЯ СПИCORОВ: Быстро собираем хэш от длины и краев
    if (PyList_Check(item)) {
        Py_ssize_t sz = PyList_GET_SIZE(item);
        Py_ssize_t list_hash = sz + 0x9e3779b9;
        if (sz > 0) {
            Py_ssize_t f_hash = PyObject_Hash(PyList_GET_ITEM(item, 0));
            if (f_hash == -1) PyErr_Clear();
            Py_ssize_t l_hash = PyObject_Hash(PyList_GET_ITEM(item, sz - 1));
            if (l_hash == -1) PyErr_Clear();
            list_hash ^= f_hash ^ l_hash;
        }
        return list_hash;
    }
    
    return (Py_ssize_t)Py_TYPE(item);
}

static void bucket_vector_push(BucketVector *vec, Py_ssize_t idx) {
    if (vec->size >= vec->capacity) {
        vec->capacity = vec->capacity == 0 ? 2 : vec->capacity * 2;
        vec->indices = (Py_ssize_t *)realloc(vec->indices, sizeof(Py_ssize_t) * vec->capacity);
    }
    vec->indices[vec->size++] = idx;
}

static inline Py_ssize_t hash_bucket_index(Py_ssize_t token, Py_ssize_t capacity) {
    size_t z = ((size_t)token) + 0x9e3779b97f4a7c15;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    return (Py_ssize_t)(z & (capacity - 1));
}

static void hash_table_rebuild(HashListObject *self);

static void hash_table_push_index(HashListObject *self, Py_ssize_t item_index, PyObject *item) {
    if (self->table_capacity == 0 || self->buckets == NULL) {
        hash_table_rebuild(self);
        if (self->table_capacity == 0 || self->buckets == NULL) {
            return;
        }
    }
    Py_ssize_t token = get_safe_token_c(item);
    Py_ssize_t bucket_idx = hash_bucket_index(token, self->table_capacity);
    self->bucket_tokens[bucket_idx] = token;
    bucket_vector_push(&self->buckets[bucket_idx], item_index);
}

static void hash_table_rebuild(HashListObject *self) {
    if (self->buckets != NULL) {
        for (Py_ssize_t i = 0; i < self->table_capacity; i++) {
            free(self->buckets[i].indices);
        }
        free(self->buckets);
        free(self->bucket_tokens);
    }
    
    Py_ssize_t capacity = 16;
    while (capacity <= self->size * 2) {
        capacity <<= 1;
    }
    
    self->buckets = (BucketVector *)calloc(capacity, sizeof(BucketVector));
    self->bucket_tokens = (Py_ssize_t *)calloc(capacity, sizeof(Py_ssize_t));
    self->table_capacity = capacity;
    self->is_dirty = 0;
    
    Py_ssize_t mask = capacity - 1;
    for (Py_ssize_t i = 0; i < self->size; i++) {
        Py_ssize_t token = get_safe_token_c(self->ob_item[i]);
        Py_ssize_t bucket_idx = hash_bucket_index(token, capacity);
        self->bucket_tokens[bucket_idx] = token;
        bucket_vector_push(&self->buckets[bucket_idx], i);
    }
}

static Py_ssize_t hash_table_lookup(HashListObject *self, PyObject *item) {
    if (self->is_dirty) {
        hash_table_rebuild(self);
    }
    if (self->table_capacity == 0 || self->size == 0) return -1;
    
    Py_ssize_t token = get_safe_token_c(item);
    Py_ssize_t bucket_idx = hash_bucket_index(token, self->table_capacity);
    BucketVector *vec = &self->buckets[bucket_idx];
    for (Py_ssize_t i = 0; i < vec->size; i++) {
        Py_ssize_t idx = vec->indices[i];
        if (idx < self->size) {
            if (fast_rich_compare_strict(self->ob_item[idx], item)) {
                return idx;
            }
        }
    }
    
    if (PyList_Check(item) || PyDict_Check(item) || PySet_Check(item)) {
        for (Py_ssize_t i = 0; i < self->size; i++) {
            if (fast_rich_compare_strict(self->ob_item[i], item)) {
                return i;
            }
        }
    }
    return -1;
}
// --- УПРАВЛЕНИЕ ПАМЯТЬЮ И МОДИФИКАЦИЯ СТРУКТУРЫ НА СИ ---

static int hashlist_resize(HashListObject *self, Py_ssize_t newsize) {
    Py_ssize_t allocated = self->allocated;
    if (allocated >= newsize && newsize >= (allocated >> 1)) {
        self->size = newsize;
        return 0;
    }
    
    Py_ssize_t new_allocated = (newsize >> 3) + (newsize < 9 ? 3 : 6) + newsize;
    if (newsize == 0) new_allocated = 0;
    
    PyObject **items = (PyObject **)realloc(self->ob_item, sizeof(PyObject *) * new_allocated);
    if (items == NULL && new_allocated > 0) {
        PyErr_NoMemory();
        return -1;
    }
    
    self->ob_item = items;
    self->size = newsize;
    self->allocated = new_allocated;
    return 0;
}

static void hashlist_dealloc(HashListObject *self) {
    if (self->ob_item != NULL) {
        for (Py_ssize_t i = 0; i < self->size; i++) {
            Py_XDECREF(self->ob_item[i]);
        }
        free(self->ob_item);
    }
    if (self->buckets != NULL) {
        for (Py_ssize_t i = 0; i < self->table_capacity; i++) {
            free(self->buckets[i].indices);
        }
        free(self->buckets);
        free(self->bucket_tokens);
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int hashlist_init(HashListObject *self, PyObject *args, PyObject *kwds) {
    PyObject *iterable = NULL;
    if (!PyArg_ParseTuple(args, "|O", &iterable)) {
        return -1;
    }
    
    self->size = 0;
    self->allocated = 0;
    self->ob_item = NULL;
    self->buckets = NULL;
    self->bucket_tokens = NULL;
    self->table_capacity = 0;
    self->is_dirty = 0;
    
    if (iterable != NULL && iterable != Py_None) {
        PyObject *iterator = PyObject_GetIter(iterable);
        if (iterator == NULL) return -1;
        
        PyObject *item;
        while ((item = PyIter_Next(iterator))) {
            if (hashlist_resize(self, self->size + 1) < 0) {
                Py_DECREF(item);
                Py_DECREF(iterator);
                return -1;
            }
            self->ob_item[self->size - 1] = item; 
        }
        Py_DECREF(iterator);
        if (PyErr_Occurred()) return -1;
    }
    
    hash_table_rebuild(self);
    return 0;
}

// --- РЕАЛИЗАЦИЯ 12 КАНОНИЧЕСКИХ МЕТОДОВ НА СИ ---

static PyObject *hashlist_append(HashListObject *self, PyObject *item) {
    if (hashlist_resize(self, self->size + 1) < 0) return NULL;

    Py_INCREF(item);
    self->ob_item[self->size - 1] = item;

    if (self->buckets != NULL && self->table_capacity > 0) {
        hash_table_push_index(self, self->size - 1, item);
        self->is_dirty = 0;
    } else self->is_dirty = 1;
    Py_RETURN_NONE;
}

static PyObject *hashlist_extend(HashListObject *self, PyObject *iterable) {
    PyObject *iterator = PyObject_GetIter(iterable);
    if (iterator == NULL) return NULL;

    PyObject *item;
    while ((item = PyIter_Next(iterator))) {
        if (hashlist_resize(self, self->size + 1) < 0) {
            Py_DECREF(item);
            Py_DECREF(iterator);
            return NULL;
        }
        self->ob_item[self->size - 1] = item;
        if (self->buckets != NULL && self->table_capacity > 0) {
            hash_table_push_index(self, self->size - 1, item);
        }
    }
    Py_DECREF(iterator);
    if (PyErr_Occurred()) return NULL;

    self->is_dirty = 0;
    Py_RETURN_NONE;
}

static PyObject *hashlist_remove(HashListObject *self, PyObject *item) {
    Py_ssize_t idx = hash_table_lookup(self, item);
    if (idx == -1) {
        PyErr_SetString(PyExc_ValueError, "hashlist.remove(x): x not in list");
        return NULL;
    }
    
    Py_DECREF(self->ob_item[idx]);
    
    for (Py_ssize_t i = idx; i < self->size - 1; i++) {
        self->ob_item[i] = self->ob_item[i + 1];
    }
    
    self->size--;
    self->is_dirty = 1; 
    
    hashlist_resize(self, self->size);
    Py_RETURN_NONE;
}

static PyObject *hashlist_pop(HashListObject *self, PyObject *args) {
    Py_ssize_t idx = -1;
    if (!PyArg_ParseTuple(args, "|n", &idx)) return NULL;
    
    if (self->size == 0) {
        PyErr_SetString(PyExc_IndexError, "pop from empty hashlist");
        return NULL;
    }
    if (idx < 0) idx += self->size;
    if (idx < 0 || idx >= self->size) {
        PyErr_SetString(PyExc_IndexError, "pop index out of range");
        return NULL;
    }
    
    PyObject *item = self->ob_item[idx];
    
    for (Py_ssize_t i = idx; i < self->size - 1; i++) {
        self->ob_item[i] = self->ob_item[i + 1];
    }
    
    self->size--;
    self->is_dirty = 1; 
    
    hashlist_resize(self, self->size);
    return item; 
}

static PyObject *hashlist_clear(HashListObject *self, PyObject *Py_UNUSED(ignored)) {
    for (Py_ssize_t i = 0; i < self->size; i++) {
        Py_DECREF(self->ob_item[i]);
    }
    hashlist_resize(self, 0);
    hash_table_rebuild(self);
    Py_RETURN_NONE;
}

static PyObject *hashlist_index_of(HashListObject *self, PyObject *item) {
    Py_ssize_t idx = hash_table_lookup(self, item);
    if (idx == -1) {
        PyErr_SetString(PyExc_ValueError, "x not in hashlist");
        return NULL;
    }
    return PyLong_FromSsize_t(idx);
}

static PyObject *hashlist_count(HashListObject *self, PyObject *item) {
    if (self->is_dirty) {
        hash_table_rebuild(self);
    }
    if (self->table_capacity == 0 || self->size == 0) return PyLong_FromLong(0);
    
    Py_ssize_t token = get_safe_token_c(item);
    Py_ssize_t bucket_idx = hash_bucket_index(token, self->table_capacity);
    BucketVector *vec = &self->buckets[bucket_idx];
    Py_ssize_t cnt = 0;
    for (Py_ssize_t i = 0; i < vec->size; i++) {
        Py_ssize_t idx = vec->indices[i];
        if (idx < self->size) { 
            // 🔥 СТРОГИЙ ПОДСЧЕТ: Исключаем ложное равенство True == 1 на Си-уровне
            if (fast_rich_compare_strict(self->ob_item[idx], item)) {
                cnt++;
            }
        }
    }
    
    if (cnt == 0 && (PyList_Check(item) || PyDict_Check(item) || PySet_Check(item))) {
        for (Py_ssize_t i = 0; i < self->size; i++) {
            if (fast_rich_compare_strict(self->ob_item[i], item)) {
                cnt++;
            }
        }
    }
    return PyLong_FromSsize_t(cnt);
}

static PyObject *hashlist_reverse(HashListObject *self, PyObject *Py_UNUSED(ignored)) {
    Py_ssize_t i = 0;
    Py_ssize_t j = self->size - 1;
    while (i < j) {
        PyObject *tmp = self->ob_item[i];
        self->ob_item[i] = self->ob_item[j];
        self->ob_item[j] = tmp;
        i++;
        j--;
    }
    hash_table_rebuild(self);
    Py_RETURN_NONE;
}

static PyObject *hashlist_contains(HashListObject *self, PyObject *item) {
    Py_ssize_t idx = hash_table_lookup(self, item);
    if (idx != -1 && idx < self->size) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

static Py_ssize_t hashlist_len(HashListObject *self) {
    return self->size;
}

static PyObject *hashlist_getitem(HashListObject *self, Py_ssize_t index) {
    if (index < 0) index += self->size;
    if (index < 0 || index >= self->size) {
        PyErr_SetString(PyExc_IndexError, "hashlist index out of range");
        return NULL;
    }
    Py_INCREF(self->ob_item[index]);
    return self->ob_item[index];
}

static PyMethodDef hashlist_methods[] = {
    {"append", (PyCFunction)hashlist_append, METH_O, "Add an item to the end"},
    {"extend", (PyCFunction)hashlist_extend, METH_O, "Extend list by appending elements from the iterable"},
    {"remove", (PyCFunction)hashlist_remove, METH_O, "Remove first occurrence of value"},
    {"pop", (PyCFunction)hashlist_pop, METH_VARARGS, "Remove and return item at index"},
    {"clear", (PyCFunction)hashlist_clear, METH_NOARGS, "Remove all items from list"},
    {"index_of", (PyCFunction)hashlist_index_of, METH_O, "Return first index of value"},
    {"count", (PyCFunction)hashlist_count, METH_O, "Return number of occurrences of value"},
    {"reverse", (PyCFunction)hashlist_reverse, METH_NOARGS, "Reverse list in-place"},
    {"contains", (PyCFunction)hashlist_contains, METH_O, "Check if item in list"},
    {NULL}
};

static PySequenceMethods hashlist_as_sequence = {
    (lenfunc)hashlist_len,                 
    0,                                     
    0,                                     
    (ssizeargfunc)hashlist_getitem,         
    0,                                     
    0,                                     
    0,                                     
    (objobjproc)hashlist_contains,         
};

static PyTypeObject HashListType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "hashlist_backend.HashListCpp",
    .tp_doc = "Industrial high-performance HashList type on pure C",
    .tp_basicsize = sizeof(HashListObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_dealloc = (destructor)hashlist_dealloc,
    .tp_init = (initproc)hashlist_init,
    .tp_methods = hashlist_methods,
    .tp_as_sequence = &hashlist_as_sequence,
    .tp_new = PyType_GenericNew,
};

static PyModuleDef hashlist_backend_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "hashlist_backend",
    .m_doc = "Pure C backend for hashlist package.",
    .m_size = -1,
};

PyMODINIT_FUNC PyInit_hashlist_backend(void) {
    PyObject *m;
    if (PyType_Ready(&HashListType) < 0) return NULL;
    
    m = PyModule_Create(&hashlist_backend_module);
    if (m == NULL) return NULL;
    
    Py_INCREF(&HashListType);
    if (PyModule_AddObject(m, "HashListCpp", (PyObject *)&HashListType) < 0) {
        Py_DECREF(&HashListType);
        Py_DECREF(m);
        return NULL;
    }
    return m;
}
