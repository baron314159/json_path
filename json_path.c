#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <yajl/yajl_parse.h>

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_json_path.h"

static PHP_MINFO_FUNCTION(json_path);

ZEND_DECLARE_MODULE_GLOBALS(json_path)

typedef struct simple_vector {
    char *elems;
    size_t elem_size;
    int len;
    int num_allocd;
} simple_vector;

typedef enum json_path_component_type {
    COMPONENT_MAP_KEY,
    COMPONENT_ARRAY_KEY
} json_path_component_type;

typedef struct json_path_component {
    json_path_component_type type;
    char wildcard;
    union {
        struct {
            char *key;
            int key_len;
        };
        int index;
    };
} json_path_component;

typedef enum json_path_type {
    TYPE_OBJECT,
    TYPE_ARRAY
} json_path_type;

typedef struct json_path_stack_elem {
    json_path_type type;
    union {
        struct {
            char *key;
            int key_len;
        };
        int index;
    };
} json_path_stack_elem;

typedef enum json_path_match_status {
    STATUS_MATCHING,
    STATUS_COLLECTING,
} json_path_match_status;

typedef struct json_path {
    simple_vector components;
    json_path_match_status status;
    char *name;
    int name_len;
    simple_vector collection_stack;
} json_path;

typedef struct json_path_object {
    zend_object zo;
    simple_vector paths;
    simple_vector path_stack;
    simple_vector callbacks;
    int objects_as_arrays;
} json_path_object;

static zend_class_entry *json_path_object_ce;
static zend_object_handlers json_path_object_handlers;

PHP_METHOD(JsonPath, addPath);
PHP_METHOD(JsonPath, getPaths);
PHP_METHOD(JsonPath, addCallback);
PHP_METHOD(JsonPath, getCallbacks);
PHP_METHOD(JsonPath, setObjectsAsArrays);
PHP_METHOD(JsonPath, getObjectsAsArrays);
PHP_METHOD(JsonPath, parse);

ZEND_BEGIN_ARG_INFO_EX(args_for_JsonPath_addPath, 0, 0, 1)
    ZEND_ARG_INFO(0, path)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(args_for_JsonPath_getPaths, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(args_for_JsonPath_addCallback, 0, 0, 1)
    ZEND_ARG_INFO(0, callback)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(args_for_JsonPath_getCallbacks, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(args_for_JsonPath_setObjectsAsArrays, 0, 0, 1)
    ZEND_ARG_INFO(0, enable)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(args_for_JsonPath_getObjectsAsArrays, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(args_for_JsonPath_parse, 0, 0, 1)
    ZEND_ARG_INFO(0, s)
ZEND_END_ARG_INFO()

static zend_function_entry json_path_object_fe[] = {
    PHP_ME(JsonPath, addPath, args_for_JsonPath_addPath, ZEND_ACC_PUBLIC)
    PHP_ME(JsonPath, getPaths, args_for_JsonPath_getPaths, ZEND_ACC_PUBLIC)
    PHP_ME(JsonPath, addCallback, args_for_JsonPath_addCallback, ZEND_ACC_PUBLIC)
    PHP_ME(JsonPath, getCallbacks, args_for_JsonPath_getCallbacks, ZEND_ACC_PUBLIC)
    PHP_ME(JsonPath, setObjectsAsArrays, args_for_JsonPath_setObjectsAsArrays, ZEND_ACC_PUBLIC)
    PHP_ME(JsonPath, getObjectsAsArrays, args_for_JsonPath_getObjectsAsArrays, ZEND_ACC_PUBLIC)
    PHP_ME(JsonPath, parse, args_for_JsonPath_parse, ZEND_ACC_PUBLIC)
    { NULL, NULL, NULL }
};

static inline void simple_vector_init(simple_vector *v, size_t elem_size)
{
    v->elem_size = elem_size;
    v->num_allocd = 5;
    v->elems = emalloc(v->elem_size * v->num_allocd);
    v->len = 0;
}

static inline void simple_vector_free(simple_vector *v)
{
    efree(v->elems);
}

static inline void simple_vector_append(simple_vector *v, void *elem)
{
    if (v->len == v->num_allocd) {
        v->num_allocd *= 2;
        v->elems = erealloc(v->elems, v->elem_size * v->num_allocd);
    }
    memcpy(&(v->elems[v->elem_size*v->len]), elem, v->elem_size);
    v->len++;
}

static inline void simple_vector_pop(simple_vector *v)
{
    v->len--;
}

#define simple_vector_get(_v, _c, _i) \
    ((_c *) &((_v)->elems[(_v)->elem_size*(_i)]))

#define simple_vector_get_last(_v, _c) \
    simple_vector_get(_v, _c, (_v)->len - 1)

static inline void json_path_free(json_path *path)
{
    int i;

    efree(path->name);

    for (i=0; i < path->components.len; i++) {
        json_path_component *c = simple_vector_get(&path->components, 
            json_path_component, i);
        if (c->type == COMPONENT_MAP_KEY) {
            efree(c->key);
        }
    }

    simple_vector_free(&path->components);
    simple_vector_free(&path->collection_stack);
}

static int json_path_parse_next(json_path *path, int head)
{
    json_path_component c;
    int tail;
    char *tmp;

    if (path->name[head] == '[') {
        head++;
        tail = head;
        while (tail < path->name_len && path->name[tail] != ']') { tail++; }
        c.type = COMPONENT_ARRAY_KEY;
        if ((tail-head) == 1 && path->name[head] == '*') {
            c.wildcard = 1;
            c.index = 0;
        } else {
            c.wildcard = 0;
            tmp = estrndup(path->name+head, (tail-head));
            c.index = atoi(tmp);
            efree(tmp);
        }
        if (tail < path->name_len) { tail++; }
    } else {
        if (path->name[head] == '.') { head++; }
        tail = head;
        while (tail < path->name_len && path->name[tail] != '.' && 
            path->name[tail] != '[') { tail++; }
        c.type = COMPONENT_MAP_KEY;
        if ((tail-head) == 1 && path->name[head] == '*') {
            c.wildcard = 1;
            c.key = NULL;
            c.key_len = 0;
        } else {
            c.wildcard = 0;
            c.key = estrndup(path->name+head, (tail-head));
            c.key_len = (tail-head);
        }
    }
    simple_vector_append(&path->components, &c);
    return tail;
}

static int json_path_parse(json_path *path)
{
    int i = 0;

    while (i < path->name_len) {
        i = json_path_parse_next(path, i);
    }

    return 1;
}

static inline void json_path_vector_free(simple_vector *paths)
{
    int i;

    for (i=0; i < paths->len; i++) {
        json_path_free(simple_vector_get(paths, json_path, i));
    }

    simple_vector_free(paths);
}

static inline void json_path_stack_free(simple_vector *path_stack)
{
    int i;

    for (i=0; i < path_stack->len; i++) {
        json_path_stack_elem *elem = simple_vector_get(path_stack, 
            json_path_stack_elem, i);
        if (elem->type == TYPE_OBJECT) {
            efree(elem->key);
        }
    }

    simple_vector_free(path_stack);
}

static inline int json_path_check_match(json_path_component *c, json_path_stack_elem *e)
{
    return (c->type == COMPONENT_ARRAY_KEY && e->type == TYPE_ARRAY 
        && (c->wildcard || e->index == c->index)) || 
        (c->type == COMPONENT_MAP_KEY && e->type == TYPE_OBJECT && 
        (c->wildcard || strcmp(e->key, c->key) == 0));
}

static void json_path_check_for_matches(json_path_object *intern)
{
    int i;

    for (i=0; i < intern->paths.len; i++) {
        json_path *curr_path = simple_vector_get(&intern->paths, json_path, i);
        if (curr_path->status == STATUS_MATCHING &&
            curr_path->components.len == intern->path_stack.len) {
            int j;

            for (j=0; j < curr_path->components.len; j++) {
                json_path_component *curr_comp = simple_vector_get(
                    &curr_path->components, json_path_component, j);
                json_path_stack_elem *curr_elem = simple_vector_get(
                    &intern->path_stack, json_path_stack_elem, j);

                if (!json_path_check_match(curr_comp, curr_elem)) {
                    break;
                }
            }

            if (j == curr_path->components.len) {
                curr_path->status = STATUS_COLLECTING;
            }
        }
    }
}

static void json_path_check_for_array_matches(json_path_object *intern)
{
    if (intern->path_stack.len > 0) {
        json_path_stack_elem *stack_elem = simple_vector_get_last(
            &intern->path_stack, json_path_stack_elem);

        if (stack_elem->type == TYPE_ARRAY) {
            stack_elem->index++;
            json_path_check_for_matches(intern);
        }
    }
}

static void json_path_append_zval(json_path_object *intern, json_path *path, zval *zv)
{
    if (path->collection_stack.len > 0) {
        json_path_stack_elem *stack_elem = simple_vector_get_last(
            &intern->path_stack, json_path_stack_elem);
        zval *outer_zv = *simple_vector_get_last(
            &path->collection_stack, zval *);

        zval_add_ref(&zv);

        if (stack_elem->type == TYPE_ARRAY) {
            add_next_index_zval(outer_zv, zv);
        } else {
            if (intern->objects_as_arrays) {
                add_assoc_zval_ex(outer_zv, stack_elem->key,
                    stack_elem->key_len+1, zv);
            } else {
                add_property_zval_ex(outer_zv, 
                    (stack_elem->key_len ? stack_elem->key : "_empty_"),
                    (stack_elem->key_len ? stack_elem->key_len+1 : sizeof("_empty_")),
                    zv);
                Z_DELREF_P(zv);
            }
        }
    }
}

static void json_path_collected_zval(json_path_object *intern, json_path *path, zval *zv)
{
    if (path->collection_stack.len == 0) {
        zval *path_zv;
        zval *retval = NULL, **argv[2];
        int i;

        for (i=0; i < intern->callbacks.len; i++) {
            zval *curr_callback = *simple_vector_get(&intern->callbacks,
                zval *, i);

            MAKE_STD_ZVAL(path_zv);
            ZVAL_STRINGL(path_zv, path->name, path->name_len, 1);

            argv[0] = &path_zv;
            argv[1] = &zv;

            if (SUCCESS == call_user_function_ex(EG(function_table),
                NULL, curr_callback, &retval, 2, argv, 0, NULL TSRMLS_CC)) {
            } else {
                if (!EG(exception)) {
                    php_error_docref(NULL TSRMLS_CC, E_WARNING, 
                        "Failed to call callback");
                }
            }

            zval_ptr_dtor(&retval);
            zval_ptr_dtor(&path_zv);
        }
    } else {
        json_path_append_zval(intern, path, zv);
    }
}

static void json_path_object_free_storage(void *object TSRMLS_DC)
{
    json_path_object *intern = (json_path_object *) object;
    int i;

    if (!intern) {
        return;
    }

    json_path_vector_free(&intern->paths);
    json_path_stack_free(&intern->path_stack);

    for (i=0; i < intern->callbacks.len; i++) {
        zval **curr_zval = simple_vector_get(&intern->callbacks, zval *, i);
        zval_ptr_dtor(curr_zval);
    }

    zend_object_std_dtor(&intern->zo TSRMLS_CC);

    efree(intern);
}

static zend_object_value json_path_object_new(zend_class_entry *class_type TSRMLS_DC)
{
    zend_object_value retval;
    json_path_object *intern;
    zval *tmp;

    intern = emalloc(sizeof(json_path_object));
    memset(&intern->zo, 0, sizeof(zend_object));

    simple_vector_init(&intern->paths, sizeof(json_path));
    intern->objects_as_arrays = 0;

    simple_vector_init(&intern->callbacks, sizeof(zval *));
    simple_vector_init(&intern->path_stack, sizeof(json_path_stack_elem));

    zend_object_std_init(&intern->zo, class_type TSRMLS_CC);
    zend_hash_copy(intern->zo.properties, 
        &class_type->default_properties, 
        (copy_ctor_func_t) zval_add_ref,
        (void *) &tmp,
        sizeof(zval *));

    retval.handle = zend_objects_store_put(intern,
        NULL,
        (zend_objects_free_object_storage_t) json_path_object_free_storage,
        NULL TSRMLS_CC);
    retval.handlers = (zend_object_handlers *) &json_path_object_handlers;

    return retval; 
}

static PHP_MINIT_FUNCTION(json_path)
{
    zend_class_entry ce;
    memset(&ce, 0, sizeof(zend_class_entry));
    INIT_CLASS_ENTRY(ce, "JsonPath", json_path_object_fe);
    ce.create_object = json_path_object_new;
    json_path_object_ce = zend_register_internal_class_ex(&ce, NULL, 
        NULL TSRMLS_CC);
    memcpy(&json_path_object_handlers, zend_get_std_object_handlers(), 
        sizeof(zend_object_handlers));

    return SUCCESS;
}

static PHP_GINIT_FUNCTION(json_path)
{

}

zend_module_entry json_path_module_entry = {
    STANDARD_MODULE_HEADER,
    "json_path",
    NULL,
    PHP_MINIT(json_path),
    NULL,
    NULL,
    NULL,
    PHP_MINFO(json_path),
    PHP_JSON_PATH_VERSION,
    PHP_MODULE_GLOBALS(json_path),
    PHP_GINIT(json_path),
    NULL,
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_JSON_PATH
ZEND_GET_MODULE(json_path)
#endif

static PHP_MINFO_FUNCTION(json_path)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "json path support", "enabled");
    php_info_print_table_row(2, "json path version", PHP_JSON_PATH_VERSION);
    php_info_print_table_end();
}

#define FETCH_THIS_AND_INTERN() \
    zval *this = getThis(); \
    json_path_object *intern = zend_object_store_get_object(this TSRMLS_CC);

PHP_METHOD(JsonPath, addPath)
{
    FETCH_THIS_AND_INTERN();
    char *name = NULL;
    int name_len = 0;
    json_path path;

    if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &name, 
        &name_len)) {
        RETURN_FALSE;
    }

    simple_vector_init(&path.components, sizeof(json_path_component));
    path.name = estrndup(name, name_len);
    path.name_len = name_len;

    simple_vector_init(&path.collection_stack, sizeof(zval *));

    path.status = STATUS_MATCHING;

    if (json_path_parse(&path)) {
        simple_vector_append(&intern->paths, &path);
        RETURN_TRUE;
    } else {
        json_path_free(&path);
        RETURN_FALSE;
    }
}

PHP_METHOD(JsonPath, getPaths)
{
    FETCH_THIS_AND_INTERN();
    int i;

    array_init(return_value);

    for (i=0; i < intern->paths.len; i++) {
        json_path *path = simple_vector_get(&intern->paths, json_path, i);
        add_next_index_string(return_value, path->name, 1);
    }
}

PHP_METHOD(JsonPath, addCallback)
{
    FETCH_THIS_AND_INTERN();
    zval *callback;

    if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", 
        &callback)) {
        RETURN_FALSE;
    }

    if (!zend_is_callable(callback, 0, NULL)) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Argument is not callable");
        RETURN_FALSE;
    }

    zval_add_ref(&callback);
    simple_vector_append(&intern->callbacks, &callback);

    RETURN_TRUE;
}

PHP_METHOD(JsonPath, getCallbacks)
{
    FETCH_THIS_AND_INTERN();
    int i;

    array_init(return_value);

    for (i=0; i < intern->paths.len; i++) {
        zval *callback = *simple_vector_get(&intern->callbacks, zval *, i);
        add_next_index_zval(return_value, callback);
    }
}

PHP_METHOD(JsonPath, setObjectsAsArrays)
{
    FETCH_THIS_AND_INTERN();
    zend_bool objects_as_arrays;

    if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "b", 
        &objects_as_arrays)) {
        RETURN_FALSE;
    }

    intern->objects_as_arrays = objects_as_arrays;

    RETURN_TRUE;
}

PHP_METHOD(JsonPath, getObjectsAsArrays)
{
    FETCH_THIS_AND_INTERN();
    RETURN_BOOL(intern->objects_as_arrays);
}

static int json_path_on_null(void *ctx)
{
    json_path_object *intern = (json_path_object *) ctx;
    int i;

    json_path_check_for_array_matches(intern);

    for (i=0; i < intern->paths.len; i++) {
        json_path *curr = simple_vector_get(&intern->paths, json_path, i);

        if (curr->status == STATUS_COLLECTING) {
            zval *zv;

            MAKE_STD_ZVAL(zv);
            ZVAL_NULL(zv);

            json_path_collected_zval(intern, curr, zv);

            zval_ptr_dtor(&zv);

            if (curr->collection_stack.len == 0) {
                curr->status = STATUS_MATCHING;
            }
        }
    }

    return 1;
}

static int json_path_on_boolean(void *ctx, int val)
{
    json_path_object *intern = (json_path_object *) ctx;
    int i;

    json_path_check_for_array_matches(intern);

    for (i=0; i < intern->paths.len; i++) {
        json_path *curr = simple_vector_get(&intern->paths, json_path, i);

        if (curr->status == STATUS_COLLECTING) {
            zval *zv;

            MAKE_STD_ZVAL(zv);
            ZVAL_BOOL(zv, val);

            json_path_collected_zval(intern, curr, zv);

            zval_ptr_dtor(&zv);

            if (curr->collection_stack.len == 0) {
                curr->status = STATUS_MATCHING;
            }
        }
    }

    return 1;
}

static int json_path_on_integer(void *ctx, long long val)
{
    json_path_object *intern = (json_path_object *) ctx;
    int i;

    json_path_check_for_array_matches(intern);

    for (i=0; i < intern->paths.len; i++) {
        json_path *curr = simple_vector_get(&intern->paths, json_path, i);

        if (curr->status == STATUS_COLLECTING) {
            zval *zv;

            MAKE_STD_ZVAL(zv);
            ZVAL_LONG(zv, (long) val);

            json_path_collected_zval(intern, curr, zv);

            zval_ptr_dtor(&zv);

            if (curr->collection_stack.len == 0) {
                curr->status = STATUS_MATCHING;
            }
        }
    }

    return 1;
}

static int json_path_on_double(void *ctx, double val)
{
    json_path_object *intern = (json_path_object *) ctx;
    int i;

    json_path_check_for_array_matches(intern);

    for (i=0; i < intern->paths.len; i++) {
        json_path *curr = simple_vector_get(&intern->paths, json_path, i);

        if (curr->status == STATUS_COLLECTING) {
            zval *zv;

            MAKE_STD_ZVAL(zv);
            ZVAL_DOUBLE(zv, val);

            json_path_collected_zval(intern, curr, zv);

            zval_ptr_dtor(&zv);

            if (curr->collection_stack.len == 0) {
                curr->status = STATUS_MATCHING;
            }
        }
    }

    return 1;
}

static int json_path_on_string(void *ctx, const unsigned char *val, size_t val_len)
{
    json_path_object *intern = (json_path_object *) ctx;
    int i;

    json_path_check_for_array_matches(intern);

    for (i=0; i < intern->paths.len; i++) {
        json_path *curr = simple_vector_get(&intern->paths, json_path, i);

        if (curr->status == STATUS_COLLECTING) {
            zval *zv;

            MAKE_STD_ZVAL(zv);
            ZVAL_STRINGL(zv, val, val_len, 1);

            json_path_collected_zval(intern, curr, zv);

            zval_ptr_dtor(&zv);

            if (curr->collection_stack.len == 0) {
                curr->status = STATUS_MATCHING;
            }
        }
    }

    return 1;
}

static int json_path_on_start_map(void *ctx)
{
    json_path_object *intern = (json_path_object *) ctx;
    json_path_stack_elem stack_elem;
    int i;

    json_path_check_for_array_matches(intern);

    for (i=0; i < intern->paths.len; i++) {
        json_path *curr = simple_vector_get(&intern->paths, json_path, i);

        if (curr->status == STATUS_COLLECTING) {
            zval *zv;

            MAKE_STD_ZVAL(zv);

            if (intern->objects_as_arrays) {
                array_init(zv);
            } else {
                object_init(zv);
            }

            simple_vector_append(&curr->collection_stack, &zv);
        }
    }

    stack_elem.type = TYPE_OBJECT;
    stack_elem.key = NULL;
    stack_elem.key_len = 0;

    simple_vector_append(&intern->path_stack, &stack_elem);

    return 1;
}

static int json_path_on_map_key(void *ctx, const unsigned char *val, size_t val_len)
{
    json_path_object *intern = (json_path_object *) ctx;
    json_path_stack_elem *stack_elem = simple_vector_get_last(
        &intern->path_stack, json_path_stack_elem);

    json_path_check_for_array_matches(intern);

    if (stack_elem->key) {
        efree(stack_elem->key);
    }

    stack_elem->key = estrndup(val, val_len);
    stack_elem->key_len = val_len;

    json_path_check_for_matches(intern);

    return 1;
}

static int json_path_on_end_map(void *ctx)
{
    json_path_object *intern = (json_path_object *) ctx;
    json_path_stack_elem *stack_elem = simple_vector_get_last(
        &intern->path_stack, json_path_stack_elem);
    int i;

    if (stack_elem->key) {
        efree(stack_elem->key);
        stack_elem->key = NULL;
    }

    simple_vector_pop(&intern->path_stack);

    for (i=0; i < intern->paths.len; i++) {
        json_path *curr = simple_vector_get(&intern->paths, json_path, i);

        if (curr->status == STATUS_COLLECTING) {
            zval *zv = *simple_vector_get_last(&curr->collection_stack, zval *);
            simple_vector_pop(&curr->collection_stack);

            json_path_collected_zval(intern, curr, zv);
            zval_ptr_dtor(&zv);

            if (curr->collection_stack.len == 0) {
                curr->status = STATUS_MATCHING;
            }
        }
    }

    return 1;
}

static int json_path_on_start_array(void *ctx)
{
    json_path_object *intern = (json_path_object *) ctx;
    json_path_stack_elem stack_elem;
    int i;

    json_path_check_for_array_matches(intern);

    for (i=0; i < intern->paths.len; i++) {
        json_path *curr = simple_vector_get(&intern->paths, json_path, i);

        if (curr->status == STATUS_COLLECTING) {
            zval *zv;

            MAKE_STD_ZVAL(zv);
            array_init(zv);

            simple_vector_append(&curr->collection_stack, &zv);
        }
    }

    stack_elem.type = TYPE_ARRAY;
    stack_elem.index = -1;

    simple_vector_append(&intern->path_stack, &stack_elem);

    return 1;
}

static int json_path_on_end_array(void *ctx)
{
    json_path_object *intern = (json_path_object *) ctx;
    json_path_stack_elem *stack_elem = simple_vector_get_last(
        &intern->path_stack, json_path_stack_elem);
    int i;

    simple_vector_pop(&intern->path_stack);

    for (i=0; i < intern->paths.len; i++) {
        json_path *curr = simple_vector_get(&intern->paths, json_path, i);

        if (curr->status == STATUS_COLLECTING) {
            zval *zv = *simple_vector_get_last(&curr->collection_stack, zval *);
            simple_vector_pop(&curr->collection_stack);

            json_path_collected_zval(intern, curr, zv);
            zval_ptr_dtor(&zv);

            if (curr->collection_stack.len == 0) {
                curr->status = STATUS_MATCHING;
            }
        }
    }

    return 1;
}

static yajl_callbacks json_path_yajl_callbacks = {
    json_path_on_null,
    json_path_on_boolean,
    json_path_on_integer,
    json_path_on_double,
    NULL,
    json_path_on_string,
    json_path_on_start_map,
    json_path_on_map_key,
    json_path_on_end_map,
    json_path_on_start_array,
    json_path_on_end_array
};

static void * json_path_yajl_malloc(void *ctx, size_t sz)
{
    return emalloc(sz);
}

static void json_path_yajl_free(void *ctx, void *ptr)
{
    efree(ptr);
}

static void * json_path_yajl_realloc(void *ctx, void *ptr, size_t sz)
{
    return erealloc(ptr, sz);
}

static yajl_alloc_funcs json_path_yajl_alloc_funcs = {
    json_path_yajl_malloc,
    json_path_yajl_realloc,
    json_path_yajl_free,
    NULL
};

static int json_path_parse_string(json_path_object *intern, char *json, int json_len)
{
    yajl_handle yh;
    yajl_status ys;

    yh = yajl_alloc(&json_path_yajl_callbacks, 
        &json_path_yajl_alloc_funcs, (void *) intern);

    ys = yajl_parse(yh, json, json_len);

    if (ys != yajl_status_ok) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, 
            "Failed parsing JSON");
        yajl_free(yh);
        return 0;
    }

    ys = yajl_complete_parse(yh);

    if (ys != yajl_status_ok) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, 
            "Failed parsing JSON");
        yajl_free(yh);
        return 0;
    }

    yajl_free(yh);

    return 1;
}

static int json_path_parse_stream(json_path_object *intern, php_stream *stream)
{
    yajl_handle yh;
    yajl_status ys;
    char buf[4096];
    size_t amt_read;

    yh = yajl_alloc(&json_path_yajl_callbacks, 
        &json_path_yajl_alloc_funcs, (void *) intern);

    while (!php_stream_eof(stream)) {
        amt_read = php_stream_read(stream, buf, sizeof(buf));

        if (amt_read < 0) {
            break;
        }

        ys = yajl_parse(yh, buf, amt_read);

        if (ys != yajl_status_ok) {
            php_error_docref(NULL TSRMLS_CC, E_WARNING, 
                "Failed parsing JSON");
            yajl_free(yh);
            return 0;
        }
    }

    ys = yajl_complete_parse(yh);

    if (ys != yajl_status_ok) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, 
            "Failed parsing JSON");
        yajl_free(yh);
        return 0;
    }

    yajl_free(yh);

    return 1;
}

PHP_METHOD(JsonPath, parse)
{
    FETCH_THIS_AND_INTERN();
    zval *z;
    php_stream *stream;

    if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &z)) {
        RETURN_FALSE;
    }

    switch (Z_TYPE_P(z)) {
        case IS_STRING:
            RETURN_BOOL(json_path_parse_string(intern,
                Z_STRVAL_P(z), Z_STRLEN_P(z)));
            break;
        case IS_RESOURCE:
            php_stream_from_zval(stream, &z);
            RETURN_BOOL(json_path_parse_stream(intern, stream));
            break;
        default:
            php_error_docref(NULL TSRMLS_CC, E_WARNING, 
                "Parameter was not a string or resource");
            RETURN_FALSE;
    }
}
