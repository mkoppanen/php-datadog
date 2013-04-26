/*
   +----------------------------------------------------------------------+
   | PHP Version 5 / datadog                                              |
   +----------------------------------------------------------------------+
   | Copyright (c) 2013 Lentor Solutions / http://lentor.io               |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Author: Mikko Kopppanen <mikko@lentor.io>                            |
   +----------------------------------------------------------------------+
*/

#ifndef _PHP_DATADOG_H_
# define _PHP_DATADOG_H_

#include "php.h"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef ZTS
# include "TSRM.h"
#endif

#define PHP_DATADOG_EXTVER "0.1.0-dev"

// Opaque struct for the timing info
typedef struct _php_datadog_timing_t php_datadog_timing_t;

typedef struct _php_datadog_transaction_t php_datadog_transaction_t;

ZEND_BEGIN_MODULE_GLOBALS(datadog)
	zend_bool enabled;			          /* is the module enabled */
	char *agent_addr;                     /* where to connect for the agent */
    char *app_name;                       /* name of the application */
    char *prefix;                         /* prefix for the metrics */

	php_datadog_timing_t *timing;           /* when the request started */
    php_datadog_transaction_t *transaction; /* currently active transaction */
    zend_bool background;                   /* if this execution is a background task */
ZEND_END_MODULE_GLOBALS(datadog)

ZEND_EXTERN_MODULE_GLOBALS(datadog)

#ifdef ZTS
#  define DATADOG_G(v) TSRMG(datadog_globals_id, zend_datadog_globals *, v)
#else
#  define DATADOG_G(v) (datadog_globals.v)
#endif

extern zend_module_entry datadog_module_entry;
#define phpext_datadog_ptr &datadog_module_entry

#endif /* _PHP_DATADOG_H_ */
