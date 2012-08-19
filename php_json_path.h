#ifndef PHP_JSON_PATH_H
#define PHP_JSON_PATH_H

#define PHP_JSON_PATH_VERSION "1.0.0"

extern zend_module_entry json_path_module_entry;
#define phpext_json_path_ptr &json_path_module_entry

#if defined(PHP_WIN32) && defined(JSON_EXPORTS)
#define PHP_JSON_PATH_API __declspec(dllexport)
#else
#define PHP_JSON_PATH_API PHPAPI
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

ZEND_BEGIN_MODULE_GLOBALS(json_path)

ZEND_END_MODULE_GLOBALS(json_path)

#ifdef ZTS
# define JSON_PATH_G(v) TSRMG(json_path_globals_id, zend_json_path_globals *, v)
#else
# define JSON_PATH_G(v) (json_path_globals.v)
#endif

#endif  /* PHP_JSON_PATH_H */
