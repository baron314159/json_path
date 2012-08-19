PHP_ARG_WITH(json_path, for JSON path support,
[  --with-json-path[=DIR]       Include JSON path support])

if test "$PHP_JSON_PATH" != "no"; then
  if test -r $PHP_JSON_PATH/include/yajl/yajl_parse.h; then
    YAJL_DIR=$PHP_JSON_PATH
  else
    AC_MSG_CHECKING(for Yajl in default path)
    for i in /usr/local /usr; do
      if test -r $i/include/yajl/yajl_parse.h; then
        YAJL_DIR=$i
        AC_MSG_RESULT(found in $i)
        break
      fi  
    done
  fi

  if test -z "$YAJL_DIR"; then
    AC_MSG_RESULT(not found)
    AC_MSG_ERROR(Please reinstall the yajl distribution -
    yajl_parse.h should be in <yajl-dir>/include/yajl/yajl_parse.h)
  fi

  AC_DEFINE([HAVE_JSON_PATH],1 , [whether to enable JSON path support])
  AC_HEADER_STDC

  PHP_ADD_INCLUDE($YAJL_DIR/include)
  PHP_ADD_LIBRARY_WITH_PATH(yajl, $YAJL_DIR/lib, JSON_PATH_SHARED_LIBADD)

  PHP_NEW_EXTENSION(json_path, json_path.c, $ext_shared)
  PHP_SUBST(JSON_PATH_SHARED_LIBADD)
fi
