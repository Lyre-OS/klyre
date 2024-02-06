#ifndef _LIB__VECTOR_K_H
#define _LIB__VECTOR_K_H

#include <stddef.h>
#include <lib/alloc.k.h>
#include <lib/libc.k.h>
#include <sys/types.h>

#define VECTOR_INVALID_INDEX (-1)

#define VECTOR_INIT {0}

#define VECTOR_TYPE(TYPE) \
    struct { \
        TYPE *data; \
        size_t length; \
        size_t capacity; \
    }

#define VECTOR_ENSURE_LENGTH(VEC, LENGTH) do { \
    __auto_type VECTOR_ENSURE_LENGTH_vec = VEC; \
    if ((LENGTH) >= VECTOR_ENSURE_LENGTH_vec->capacity) { \
        if (VECTOR_ENSURE_LENGTH_vec->capacity == 0) { \
            VECTOR_ENSURE_LENGTH_vec->capacity = 8; \
        } else { \
            VECTOR_ENSURE_LENGTH_vec->capacity *= 2; \
        } \
        VECTOR_ENSURE_LENGTH_vec->data = realloc(VECTOR_ENSURE_LENGTH_vec->data, \
            VECTOR_ENSURE_LENGTH_vec->capacity * sizeof(*VECTOR_ENSURE_LENGTH_vec->data)); /* NOLINT */ \
    } \
} while (0)

#define VECTOR_PUSH_BACK(VEC, VALUE) ({ \
    __auto_type VECTOR_PUSH_BACK_vec = VEC; \
    VECTOR_ENSURE_LENGTH(VEC, VECTOR_PUSH_BACK_vec->length); \
    VECTOR_PUSH_BACK_vec->data[VECTOR_PUSH_BACK_vec->length++] = VALUE; \
    VECTOR_PUSH_BACK_vec->length - 1; \
})

#define VECTOR_PUSH_FRONT(VEC, VALUE) VECTOR_INSERT(VEC, 0, VALUE)

#define VECTOR_INSERT(VEC, IDX, VALUE) do { \
    __auto_type VECTOR_INSERT_vec = VEC; \
    size_t VECTOR_INSERT_index = IDX; \
    VECTOR_ENSURE_LENGTH(VEC, VECTOR_INSERT_vec->length); \
    for (size_t VECTOR_INSERT_i = VECTOR_INSERT_vec->length; VECTOR_INSERT_i > VECTOR_INSERT_index; VECTOR_INSERT_i--) { \
        VECTOR_INSERT_vec->data[VECTOR_INSERT_i] = VECTOR_INSERT_vec->data[VECTOR_INSERT_i - 1]; \
    } \
    VECTOR_INSERT_vec->length++; \
    VECTOR_INSERT_vec->data[VECTOR_INSERT_index] = VALUE; \
} while (0)

#define VECTOR_REMOVE(VEC, IDX) do { \
    __auto_type VECTOR_REMOVE_vec = VEC; \
    for (size_t VECTOR_REMOVE_i = (IDX); VECTOR_REMOVE_i < VECTOR_REMOVE_vec->length - 1; VECTOR_REMOVE_i++) { \
        VECTOR_REMOVE_vec->data[VECTOR_REMOVE_i] = VECTOR_REMOVE_vec->data[VECTOR_REMOVE_i + 1]; \
    } \
    VECTOR_REMOVE_vec->length--; \
} while (0)

#define VECTOR_ITEM(VEC, IDX) ({ \
    size_t VECTOR_ITEM_idx = IDX; \
    __auto_type VECTOR_ITEM_vec = VEC; \
    __auto_type VECTOR_ITEM_result = (typeof(*VECTOR_ITEM_vec->data))VECTOR_INVALID_INDEX; \
    if (VECTOR_ITEM_idx < VECTOR_ITEM_vec->length) { \
        VECTOR_ITEM_result = VECTOR_ITEM_vec->data[VECTOR_ITEM_idx]; \
    } \
    VECTOR_ITEM_result; \
})

#define VECTOR_FIND(VEC, VALUE) ({ \
    __auto_type VECTOR_FIND_vec = VEC; \
    ssize_t VECTOR_FIND_result = VECTOR_INVALID_INDEX; \
    for (size_t VECTOR_FIND_i = 0; VECTOR_FIND_i < VECTOR_FIND_vec->length; VECTOR_FIND_i++) { \
        if (VECTOR_FIND_vec->data[VECTOR_FIND_i] == (VALUE)) { \
            VECTOR_FIND_result = VECTOR_FIND_i; \
            break; \
        } \
    } \
    VECTOR_FIND_result; \
})

#define VECTOR_REMOVE_BY_VALUE(VEC, VALUE) do { \
    __auto_type VECTOR_REMOVE_BY_VALUE_vec = VEC; \
    __auto_type VECTOR_REMOVE_BY_VALUE_v = VALUE; \
    size_t VECTOR_REMOVE_BY_VALUE_i = VECTOR_FIND(VECTOR_REMOVE_BY_VALUE_vec, VECTOR_REMOVE_BY_VALUE_v); \
    VECTOR_REMOVE(VECTOR_REMOVE_BY_VALUE_vec, VECTOR_REMOVE_BY_VALUE_i); \
} while (0)

#define VECTOR_FOR_EACH(VEC, BINDING, ...) do { \
    __auto_type VECTOR_FOR_EACH_vec = VEC; \
    for (size_t VECTOR_FOR_EACH_i = 0; VECTOR_FOR_EACH_i < VECTOR_FOR_EACH_vec->length; VECTOR_FOR_EACH_i++) { \
        __auto_type BINDING = &VECTOR_FOR_EACH_vec->data[VECTOR_FOR_EACH_i]; \
        __VA_ARGS__ \
    } \
} while (0)

#endif
