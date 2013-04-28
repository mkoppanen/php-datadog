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

#include "php_datadog.h"

#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/standard/php_smart_str.h"
#include "ext/standard/php_string.h"
#include "SAPI.h"

// getrusage
#include <sys/resource.h>

// Needed for times syscall
# ifndef timersub
#define timersub(tvp, uvp, vvp) do { \
        (vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec; \
        (vvp)->tv_usec = (tvp)->tv_usec - (uvp)->tv_usec; \
        if ((vvp)->tv_usec < 0) { \
            (vvp)->tv_sec--; \
            (vvp)->tv_usec += 1000000; \
        } \
    } while (0)
# endif /* ifndef timersub */

#define timeval_to_msec(_my_tv) ((_my_tv.tv_sec * 1000) + (_my_tv.tv_usec / 1000))

// Bring in the globals
ZEND_DECLARE_MODULE_GLOBALS(datadog)

// Contains request times and CPU usage
struct _php_datadog_timing_t {
    struct timeval st_tv;
    struct rusage  st_ru;
};

// Contains transaction
struct _php_datadog_transaction_t {
    php_datadog_timing_t *timing;
    zval *tags;
    size_t st_mem;
};

static
void s_smart_str_append_tag (smart_str *str, const char *name, const char *value)
{
    smart_str_appendc (str, '#');
    smart_str_appends (str, name);
    smart_str_appendc (str, ':');
    smart_str_appends (str, value);
    smart_str_appendc (str, ',');
}

static
char *s_request_tags (TSRMLS_D)
{
    char *retval;
    smart_str tags = {0};

    if (SG (request_info).path_translated) {
        char *filename;
        size_t filename_len;

        // Get the basename of the script
        php_basename (SG (request_info).path_translated, strlen (SG (request_info).path_translated),
            NULL, 0, &filename, &filename_len TSRMLS_CC);

        if (filename) {
            s_smart_str_append_tag (&tags, "filename", filename);
            efree (filename);
        }
    } else
        // No filename
        s_smart_str_append_tag (&tags, "filename", "-");

    if (DATADOG_G (app_name))
        s_smart_str_append_tag (&tags, "application", DATADOG_G (app_name));

    if (DATADOG_G (background))
        s_smart_str_append_tag (&tags, "background", "yes");

    // Terminate the string
    smart_str_0 (&tags);
    retval = pestrdup (tags.c, 1);

    smart_str_free (&tags);
    return retval;
}

static
int s_append_tags_fn (void *pce TSRMLS_DC, int num_args, va_list args, zend_hash_key *hash_key)
{
    int key_len;
    const char *key;
    char key_buf[48];

    if (hash_key->nKeyLength == 0) {
        snprintf (key_buf, 48, "%ld", hash_key->h);
        key = key_buf;
    } else {
        key = hash_key->arKey;
    }

    smart_str *metric = va_arg (args, smart_str *);
    s_smart_str_append_tag (metric, key, Z_STRVAL_PP ((zval **) pce));
    return ZEND_HASH_APPLY_KEEP;
}

static
void s_append_tags (smart_str *metric, zval *tags TSRMLS_DC)
{
    // Apply function
    zend_hash_apply_with_arguments (Z_ARRVAL_P(tags) TSRMLS_CC, s_append_tags_fn, 1, metric);
}

static
php_stream *s_datadog_get_stream (const char *addr, size_t addr_len TSRMLS_DC)
{
    struct timeval tv;

    php_stream *stream;
    char *err_msg = NULL;
    char persistent_id [96];
    int err_code = 0;

    snprintf (persistent_id, 96, "datadog:%s", addr);

    // TODO: hardcoded
    tv.tv_sec  = 10;
    tv.tv_usec = 0;

    stream = php_stream_xport_create (addr, addr_len,
                                      0, STREAM_XPORT_CLIENT | STREAM_XPORT_CONNECT,
                                      persistent_id, &tv, NULL, &err_msg, &err_code);

    if (!stream)
        return NULL;

    if (php_stream_set_option (stream, PHP_STREAM_OPTION_BLOCKING, 0, NULL) == -1) {
        php_stream_close (stream);
        return NULL;
    }
    php_stream_auto_cleanup (stream);
    return stream;
}

static
zend_bool s_do_send (smart_str *metric TSRMLS_DC)
{
    smart_str final = {0};
    php_stream *stream;

    // Finalise
    smart_str_0 (metric);

    // Get a socket handle
    stream = s_datadog_get_stream (DATADOG_G (agent_addr), strlen (DATADOG_G (agent_addr)) TSRMLS_CC);

    if (!stream)
        return 0;

#ifdef mikko_0
    printf ("metric=[%s]\n", metric->c);
#endif

    // TODO: add logic here where transaction / request stuff gets broken into less than 512 bytes

    // Send the metric
    return (php_stream_write (stream, metric->c, metric->len) == metric->len);
}

static
void s_generate_metric (smart_str *metric, const char *prefix, const char *name, long value, const char *unit, zval *tags TSRMLS_DC)
{
    // Global prefix
    if (DATADOG_G (prefix) && strlen (DATADOG_G (prefix))) {
        // Set prefix
        smart_str_appends (metric, DATADOG_G (prefix));
    }

    // sub-prefix, makes using same func from request and transaction easier
    if (prefix) {
        smart_str_appends (metric, prefix);
        smart_str_appendc (metric, '.');
    }

    smart_str_appends (metric, name);
    smart_str_appendc (metric, ':');

    smart_str_append_long (metric, value);
    smart_str_appendc (metric, '|');
    smart_str_appends (metric, unit);

    // First append standard tags
    smart_str_appendc (metric, '|');

    // Append request tags
    smart_str_appends (metric, DATADOG_G (request_tags));

    // And user tags
    if (tags) {
        if (Z_TYPE_P (tags) == IS_ARRAY) {
            if (zend_hash_num_elements (Z_ARRVAL_P (tags)))
                s_append_tags (metric, tags TSRMLS_CC);
        }
        else if (Z_TYPE_P (tags) == IS_STRING) {
            smart_str_appends (metric, Z_STRVAL_P (tags));
        }
    }
    // Remove trailing comma if any
    if (metric->c [metric->len - 1] == ',') {
        metric->len = metric->len - 1;
    }
}

static
zend_bool s_send_metric (const char *name, long value, const char *unit, zval *tags TSRMLS_DC)
{
    zend_bool rc;
    smart_str metric = { 0 };
    s_generate_metric (&metric, NULL, name, value, unit, tags TSRMLS_CC);

    rc = s_do_send (&metric TSRMLS_CC);
    smart_str_free (&metric);

    return rc;
}

static
void s_send_transaction (php_datadog_timing_t *timing, const char *prefix, zval *tags TSRMLS_DC)
{
    // End of request status
    struct timeval en_tv, real_tv;
    struct rusage  en_ru;
    smart_str tr_end = {0};

    // Amount in milliseconds used
    if (gettimeofday (&en_tv, NULL) == 0) {
        timersub (&en_tv, &(timing->st_tv), &real_tv);

        s_generate_metric (&tr_end, prefix, "time.real", timeval_to_msec (real_tv), "ms", tags TSRMLS_CC);
        smart_str_appendc (&tr_end, '\n');
    }

    // CPU usage
    if (getrusage (RUSAGE_SELF, &en_ru) == 0) {
        struct timeval tv_utime, tv_stime;

        timersub (&en_ru.ru_utime, &(timing->st_ru.ru_utime), &tv_utime);
        timersub (&en_ru.ru_stime, &(timing->st_ru.ru_stime), &tv_stime);

        s_generate_metric (&tr_end, prefix, "time.user", timeval_to_msec (tv_utime), "ms", tags TSRMLS_CC);
        smart_str_appendc (&tr_end, '\n');

        s_generate_metric (&tr_end, prefix, "time.sys",  timeval_to_msec (tv_stime), "ms", tags TSRMLS_CC);
        smart_str_appendc (&tr_end, '\n');
    }
    // Send end of request statistics
    s_do_send (&tr_end TSRMLS_CC);
    smart_str_free (&tr_end);
}

// Initialises the datadog transaction
static
php_datadog_timing_t *s_datadog_timing (TSRMLS_D)
{
    php_datadog_timing_t *timing;

    if (!DATADOG_G (enabled))
        return NULL; // Not enabled

    timing = pemalloc (sizeof (php_datadog_timing_t), 1);

    if (gettimeofday (&timing->st_tv, NULL) != 0) {
        // TODO: handle fail
    }

    if (getrusage (RUSAGE_SELF, &timing->st_ru) != 0) {
        // TODO: handle fail
    }
    return timing;
}

/* {{{ boolean datadog_set_background (boolean $background)
    Mark this script as background task
*/
PHP_FUNCTION(datadog_set_background)
{
    zend_bool rc, background;

    if (zend_parse_parameters (ZEND_NUM_ARGS() TSRMLS_CC, "b", &background) != SUCCESS) {
        return;
    }
    rc = DATADOG_G (background);
    DATADOG_G (background) = background;

    RETURN_BOOL(rc);
}
/* }}} */

// Implementation of metrics
static
void s_datadog_metric_collection (INTERNAL_FUNCTION_PARAMETERS, const char *type)
{
    char *name;
    int name_len;
    long value;
    zval *tags = NULL;
    zend_bool retval;

    if (zend_parse_parameters (ZEND_NUM_ARGS() TSRMLS_CC, "sl|a!", &name, &name_len, &value, &tags) != SUCCESS) {
        return;
    }

    if (!DATADOG_G (enabled)) {
        RETURN_FALSE;
    }

    retval = s_send_metric (name, value, type, tags TSRMLS_CC);
    RETVAL_BOOL (retval);
}

// Implementation of metrics
static
void s_datadog_incr_decr (INTERNAL_FUNCTION_PARAMETERS, int value)
{
    char *name;
    int name_len;
    zval *tags = NULL;
    zend_bool retval;

    if (zend_parse_parameters (ZEND_NUM_ARGS() TSRMLS_CC, "s|a!", &name, &name_len, &tags) != SUCCESS) {
        return;
    }

    if (!DATADOG_G (enabled)) {
        RETURN_FALSE;
    }

    retval = s_send_metric (name, value, "c", tags TSRMLS_CC);
    RETVAL_BOOL (retval);
}

/* {{{ boolean datadog_timing(string $name, int $milliseconds[, array $tags = array ()])
    Create a timing for a specific entry
*/
PHP_FUNCTION(datadog_timing)
{
    s_datadog_metric_collection (INTERNAL_FUNCTION_PARAM_PASSTHRU, "ms");
}
/* }}} */

/* {{{ boolean datadog_gauge(string $name, int $value[, array $tags = array ()])
    Create a gauge for a specific entry
*/
PHP_FUNCTION(datadog_gauge)
{
    s_datadog_metric_collection (INTERNAL_FUNCTION_PARAM_PASSTHRU, "g");
}
/* }}} */

/* {{{ boolean datadog_histogram(string $name, int $value[, array $tags = array ()])
    Create a history for a specific entry
*/
PHP_FUNCTION(datadog_histogram)
{
    s_datadog_metric_collection (INTERNAL_FUNCTION_PARAM_PASSTHRU, "h");
}
/* }}} */

/* {{{ boolean datadog_increment(string $name[, array $tags = array ()])
    Increment a counter
*/
PHP_FUNCTION(datadog_increment)
{
    s_datadog_incr_decr (INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}
/* }}} */

/* {{{ boolean datadog_decrement(string $name[, array $tags = array ()])
    Decrement a counter
*/
PHP_FUNCTION(datadog_decrement)
{
    s_datadog_incr_decr (INTERNAL_FUNCTION_PARAM_PASSTHRU, -1);
}
/* }}} */

/* {{{ boolean datadog_transaction_begin(string $name[, array $tags = array ()])
    Create a timing for a specific transaction
*/
PHP_FUNCTION(datadog_transaction_begin)
{
    char *name;
    int name_len;
    zval *tags = NULL;

    if (zend_parse_parameters (ZEND_NUM_ARGS() TSRMLS_CC, "s|a!", &name, &name_len, &tags) != SUCCESS) {
        return;
    }

    if (!DATADOG_G (enabled)) {
        RETURN_FALSE;
    }

    // There is already a transaction
    if (DATADOG_G (transaction)) {
        RETURN_FALSE;
    }

    // Allocate new transaction
    DATADOG_G (transaction)         = pemalloc (sizeof (php_datadog_transaction_t), 1);
    DATADOG_G (transaction)->timing = s_datadog_timing (TSRMLS_C);
    DATADOG_G (transaction)->st_mem = zend_memory_usage (1 TSRMLS_CC);

    MAKE_STD_ZVAL (DATADOG_G (transaction)->tags);

    if (tags) {
        ZVAL_ZVAL (DATADOG_G (transaction)->tags, tags, 1, 0);
    }
    else {
        array_init (DATADOG_G (transaction)->tags);
    }

    add_assoc_string (DATADOG_G (transaction)->tags, "transaction", name, 1);
    Z_ADDREF_P (DATADOG_G (transaction)->tags);

    RETURN_TRUE;
}
/* }}} */

/* {{{ boolean datadog_transaction_end([boolean $discard = false])
    End currently active transaction
*/
PHP_FUNCTION(datadog_transaction_end)
{
    zend_bool discard = 0;

    if (zend_parse_parameters (ZEND_NUM_ARGS() TSRMLS_CC, "|b", &discard) != SUCCESS) {
        return;
    }

    if (!DATADOG_G (enabled)) {
        RETURN_FALSE;
    }

    if (!DATADOG_G (transaction)) {
        RETURN_FALSE;
    }

    if (!discard) {
        long memory_usage;
        s_send_transaction (DATADOG_G (transaction)->timing, "transaction", DATADOG_G (transaction)->tags TSRMLS_CC);

        memory_usage = (zend_memory_usage (1 TSRMLS_CC) - DATADOG_G (transaction)->st_mem);
        s_send_metric ("transaction.memory.usage", memory_usage, "g", DATADOG_G (transaction)->tags TSRMLS_CC);
    }

    Z_DELREF_P (DATADOG_G (transaction)->tags);
    zval_ptr_dtor (&(DATADOG_G (transaction)->tags));

    free (DATADOG_G (transaction)->timing);
    pefree (DATADOG_G (transaction), 1);

    DATADOG_G (transaction) = NULL;
    RETURN_TRUE;
}
/* }}} */

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("datadog.enabled",     "1",                    PHP_INI_PERDIR, OnUpdateBool,   enabled,    zend_datadog_globals, datadog_globals)
    STD_PHP_INI_ENTRY("datadog.agent",       "udp://127.0.0.1:8125", PHP_INI_PERDIR, OnUpdateString, agent_addr, zend_datadog_globals, datadog_globals)
    STD_PHP_INI_ENTRY("datadog.application", "default",              PHP_INI_PERDIR, OnUpdateString, app_name,   zend_datadog_globals, datadog_globals)
    STD_PHP_INI_ENTRY("datadog.prefix",      "php.",                 PHP_INI_PERDIR, OnUpdateString, prefix,     zend_datadog_globals, datadog_globals)
PHP_INI_END()

static
void s_datadog_capture_error (int type, const char *error_filename, const uint error_lineno, const char *format, va_list args)
{
    zval *tags;
    const char *pretty_tag = NULL;

    switch (type) {
        case E_ERROR:
            pretty_tag = "#level:E_ERROR";
        break;

        case E_CORE_ERROR:
            pretty_tag = "#level:E_CORE_ERROR";
        break;

        case E_COMPILE_ERROR:
            pretty_tag = "#level:E_COMPILE_ERROR";
        break;

        case E_USER_ERROR:
            pretty_tag = "#level:E_USER_ERROR";
        break;

        case E_RECOVERABLE_ERROR:
            pretty_tag = "#level:E_RECOVERABLE_ERROR";
        break;

        case E_CORE_WARNING:
            pretty_tag = "#level:E_CORE_WARNING";
        break;

        case E_COMPILE_WARNING:
            pretty_tag = "#level:E_COMPILE_WARNING";
        break;

        case E_USER_WARNING:
            pretty_tag = "#level:E_USER_WARNING";
        break;

        case E_PARSE:
            pretty_tag = "#level:E_PARSE";
        break;

        case E_NOTICE:
            pretty_tag = "#level:E_NOTICE";
        break;

        case E_USER_NOTICE:
            pretty_tag = "#level:E_USER_NOTICE";
        break;

        case E_STRICT:
            pretty_tag = "#level:E_STRICT";
        break;

        case E_DEPRECATED:
            pretty_tag = "#level:E_DEPRECATED";
        break;

        case E_USER_DEPRECATED:
            pretty_tag = "#level:E_USER_DEPRECATED";
        break;

        default:
            pretty_tag = "#level:UNKNOWN";
        break;
    }

    // Create a gauge out of the errors
    // TODO: add a threshold here after which move into sampling
    MAKE_STD_ZVAL (tags);
    ZVAL_STRING (tags, pretty_tag, 1);

    TSRMLS_FETCH ();
    s_send_metric ("error.reporting", 1, "c", tags TSRMLS_CC);
    zval_ptr_dtor (&tags);

    // pass through to the original error callback
    DATADOG_G (zend_error_cb) (type, error_filename, error_lineno, format, args);
}

static
void s_datadog_override_error_handler (TSRMLS_D) 
{
    DATADOG_G (zend_error_cb) = zend_error_cb;
    zend_error_cb = &s_datadog_capture_error;
}

PHP_RINIT_FUNCTION(datadog)
{
    if (DATADOG_G (enabled)) {
        // The request tags
        DATADOG_G (request_tags) = s_request_tags (TSRMLS_C);

        // Override error handling
        s_datadog_override_error_handler (TSRMLS_C);

        // Init datadog, main timer
        DATADOG_G (timing) = s_datadog_timing (TSRMLS_C);
    }
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(datadog)
{
    if (DATADOG_G (enabled)) {
        php_datadog_timing_t *timing = DATADOG_G (timing);

        if (timing) {
            s_send_transaction (timing, "request", NULL TSRMLS_CC);

            long peak_memory = zend_memory_peak_usage (1 TSRMLS_CC);
            s_send_metric ("request.memory.peak", peak_memory, "g", NULL TSRMLS_CC);

            pefree (timing, 1);
        }

        if (DATADOG_G (transaction)) {
            // Open transaction, close it
            zval_ptr_dtor (&(DATADOG_G (transaction)->tags));

            pefree (DATADOG_G (transaction)->timing, 1);
            pefree (DATADOG_G (transaction), 1);

            DATADOG_G (transaction) = NULL;
        }

        if (DATADOG_G (request_tags))
            pefree (DATADOG_G (request_tags), 1);
    }
    return SUCCESS;
}

/* {{{ PHP_MINIT_FUNCTION(datadog) */
PHP_MINIT_FUNCTION(datadog)
{
    REGISTER_INI_ENTRIES();
    return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION(datadog) */
PHP_MSHUTDOWN_FUNCTION(datadog)
{
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}
/* }}} */

PHP_GINIT_FUNCTION(datadog)
{
    datadog_globals->background     = 0;
    datadog_globals->transaction = NULL;
}

/* {{{ PHP_MINFO_FUNCTION(datadog) */
PHP_MINFO_FUNCTION(datadog)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "datadog extension", "enabled");
    php_info_print_table_row(2, "datadog version", PHP_DATADOG_EXTVER);
    php_info_print_table_end();

    DISPLAY_INI_ENTRIES();
}

static zend_function_entry datadog_functions[] = {
    PHP_FE (datadog_timing,            NULL)
    PHP_FE (datadog_gauge,             NULL)
    PHP_FE (datadog_histogram,         NULL)
    PHP_FE (datadog_increment,         NULL)
    PHP_FE (datadog_decrement,         NULL)
    PHP_FE (datadog_set_background,    NULL)
    PHP_FE (datadog_transaction_begin, NULL)
    PHP_FE (datadog_transaction_end,   NULL)
    {NULL, NULL, NULL}
};

zend_module_entry datadog_module_entry = {
    STANDARD_MODULE_HEADER,
    "datadog",
    datadog_functions,
    PHP_MINIT(datadog),
    PHP_MSHUTDOWN(datadog),
    PHP_RINIT(datadog),
    PHP_RSHUTDOWN(datadog),
    PHP_MINFO(datadog),
    PHP_DATADOG_EXTVER,
    PHP_MODULE_GLOBALS(datadog),
    PHP_GINIT(datadog),
    NULL, /* GSHUTDOWN */
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_DATADOG
ZEND_GET_MODULE(datadog)
#endif