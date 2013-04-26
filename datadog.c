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
zval *s_request_tags ()
{
    zval *tags;
    const char *f;

    MAKE_STD_ZVAL (tags);
    array_init (tags);

    if (SG (request_info).path_translated) {
        char *filename;
        size_t filename_len;

        // Get the basename of the script
        php_basename (SG (request_info).path_translated, strlen (SG (request_info).path_translated),
            NULL, 0, &filename, &filename_len TSRMLS_CC);

        if (filename)
            add_assoc_string (tags, "filename", filename, 0);
    } else {
        // No filename
        add_assoc_string (tags, "filename", "-", 1);
    }

    if (DATADOG_G(app_name))
        add_assoc_string (tags, "application", DATADOG_G (app_name), 1);

    if (DATADOG_G (background))
        add_assoc_string (tags, "background", "yes", 1);

    return tags;
}

static
int s_append_tags_fn (void *pce TSRMLS_DC, int num_args, va_list args, zend_hash_key *hash_key)
{
    int key_len;
    char *key, key_buf[48];

    if (hash_key->nKeyLength == 0) {
        snprintf (key_buf, 48, "%ld", hash_key->h);
        key = key_buf;
    } else {
        key = hash_key->arKey;
    }

    smart_str *metric = va_arg (args, smart_str *);

    smart_str_appendc (metric, '#');
    smart_str_appends (metric, key);
    smart_str_appendc (metric, ':');

    // Append the value
    smart_str_appends (metric, Z_STRVAL_PP ((zval **) pce));
    smart_str_appendc (metric, ',');

    return ZEND_HASH_APPLY_KEEP;
}

static
void s_append_tags (smart_str *metric, zval *tags TSRMLS_DC)
{
    // Apply function
    zend_hash_apply_with_arguments (Z_ARRVAL_P(tags) TSRMLS_CC, s_append_tags_fn, 1, metric);
}

static
php_stream *s_datadog_get_stream (const char *addr, size_t addr_len)
{
    struct timeval tv;

    php_stream *stream;
    char *err_msg;
    char persistent_id [96];
    int err_code;

    snprintf (persistent_id, 96, "datadog:%s", addr);

    stream = php_stream_xport_create (addr, addr_len,
                                      ~REPORT_ERRORS, STREAM_XPORT_CLIENT | STREAM_XPORT_CONNECT,
                                      persistent_id, &tv, NULL, &err_msg, &err_code);

    if (!stream)
        return NULL;

    if (php_stream_set_option (stream, PHP_STREAM_OPTION_BLOCKING, 0, NULL) == -1) {
        php_stream_close (stream);
        return NULL;
    }
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
    stream = s_datadog_get_stream (DATADOG_G (agent_addr), strlen (DATADOG_G (agent_addr)));

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
    zval *request_tags;

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

    request_tags = s_request_tags ();
    s_append_tags (metric, request_tags TSRMLS_CC);

    // And user tags
    if (tags) {
        if (Z_TYPE_P (tags) == IS_ARRAY) {
            if (zend_hash_num_elements (Z_ARRVAL_P (tags)))
                s_append_tags (metric, tags TSRMLS_CC);
        }
        else if (Z_TYPE_P (tags) == IS_STRING) {
            smart_str_appendc (metric, '|');
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
    s_do_send (&tr_end);
    smart_str_free (&tr_end);
}

// Initialises the datadog transaction
static
php_datadog_timing_t *s_datadog_timing ()
{
    php_datadog_timing_t *timing;

    if (!DATADOG_G (enabled))
        return NULL; // Not enabled

    timing = calloc (1, sizeof (php_datadog_timing_t));

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

/* {{{ datadog_timing(string $name, int $milliseconds[, array $tags = array ()])
    Create a timing for a specific entry
*/
PHP_FUNCTION(datadog_timing)
{
    char *name;
    int name_len;
    long milliseconds;
    zval *tags = NULL;
    zend_bool retval;

    if (zend_parse_parameters (ZEND_NUM_ARGS() TSRMLS_CC, "sl|a!", &name, &name_len, &milliseconds, &tags) != SUCCESS) {
        return;
    }
    retval = s_send_metric (name, milliseconds, "ms", tags TSRMLS_CC);
    RETVAL_BOOL (retval);
}
/* }}} */

/* {{{ datadog_transaction_begin(string $name[, array $tags = array ()])
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

    // There is already a transaction
    if (DATADOG_G (transaction)) {
        RETURN_FALSE;
    }

    // Allocate new transaction
    DATADOG_G (transaction)         = emalloc (sizeof (php_datadog_transaction_t));
    DATADOG_G (transaction)->timing = s_datadog_timing ();
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

/* {{{ datadog_transaction_end([boolean $discard = false])
    End currently active transaction
*/
PHP_FUNCTION(datadog_transaction_end)
{
    zend_bool discard = 0;

    if (zend_parse_parameters (ZEND_NUM_ARGS() TSRMLS_CC, "|b", &discard) != SUCCESS) {
        return;
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

    if (DATADOG_G (transaction)->tags) {
        Z_DELREF_P (DATADOG_G (transaction)->tags);
    }

    free (DATADOG_G (transaction)->timing);
    efree (DATADOG_G (transaction));

    DATADOG_G (transaction) = NULL;
}
/* }}} */

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("datadog.enabled",     "1",                     PHP_INI_PERDIR,  OnUpdateBool,    enabled,     zend_datadog_globals, datadog_globals)
    STD_PHP_INI_ENTRY("datadog.agent",       "udp://127.0.0.1:8125",  PHP_INI_PERDIR,  OnUpdateString,  agent_addr,  zend_datadog_globals, datadog_globals)
    STD_PHP_INI_ENTRY("datadog.application", "default",               PHP_INI_PERDIR,  OnUpdateString,  app_name,    zend_datadog_globals, datadog_globals)
    STD_PHP_INI_ENTRY("datadog.prefix",      "php.",                   PHP_INI_PERDIR,  OnUpdateString,  prefix,      zend_datadog_globals, datadog_globals)
PHP_INI_END()

PHP_RINIT_FUNCTION(datadog)
{
    // Init datadog, main timer
    DATADOG_G (timing) = s_datadog_timing ();
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(datadog)
{
    php_datadog_timing_t *timing = DATADOG_G (timing);

    if (timing) {
        s_send_transaction (timing, "request", NULL TSRMLS_CC);

        long peak_memory = zend_memory_peak_usage (1 TSRMLS_CC);
        s_send_metric ("request.memory.peak", peak_memory, "g", NULL TSRMLS_CC);
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
    PHP_GINIT(datadog), /* GINIT */
    NULL, /* GSHUTDOWN */
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_DATADOG
ZEND_GET_MODULE(datadog)
#endif