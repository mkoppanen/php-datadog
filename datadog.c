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
#include "ext/standard/url.h"
#include "ext/standard/info.h"
#include "ext/standard/php_smart_str.h"
#include "ext/standard/php_string.h"
#include "ext/standard/php_rand.h"
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

#define timeval_to_msec(_my_tv) ((_my_tv.tv_sec * 1000.0) + (_my_tv.tv_usec / 1000.0))

#define timeval_to_double(_my_tv) (double)(_my_tv).tv_sec + ((double)(_my_tv).tv_usec / 1000000.0)

/* The original error callback, there seems to be no typedef for this */
static
	void (*orig_zend_error_cb) (int type, const char *error_filename, const uint error_lineno, const char *format, va_list args);


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
    double sample_rate;
};

static
int s_check_sample_rate (double sample_rate TSRMLS_DC)
{
    return ((php_rand (TSRMLS_C) / PHP_RAND_MAX) <= sample_rate);
}

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

    // And request_uri
    if (SG (request_info).request_uri) {
        if (DATADOG_G (strip_query) && strchr (SG (request_info).request_uri, '?')) {
            php_url *url = php_url_parse (SG (request_info).request_uri);

            if (url) {
                s_smart_str_append_tag (&tags, "request_uri", url->path);
                php_url_free (url);
            }
        } else {
            s_smart_str_append_tag (&tags, "request_uri", SG (request_info).request_uri);
        }
    }

    // Requested filename
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
void s_metric_header (smart_str *metric, const char *sub_prefix TSRMLS_DC)
{
    // Global prefix
    if (DATADOG_G (prefix) && strlen (DATADOG_G (prefix))) {
        // Set prefix
        smart_str_appends (metric, DATADOG_G (prefix));
    }

    // sub-prefix, makes using same func from request and transaction easier
    if (sub_prefix) {
        smart_str_appends (metric, sub_prefix);
        smart_str_appendc (metric, '.');
    }
}

static
void s_metric_footer (smart_str *metric, zval *tags TSRMLS_DC)
{
    // Append request tags
    smart_str_appends (metric, DATADOG_G (request_tags));

    // Background job?
    if (DATADOG_G (background))
        s_smart_str_append_tag (metric, "background", "yes");

    // Application name
    if (DATADOG_G (app_name))
        s_smart_str_append_tag (metric, "application", DATADOG_G (app_name));

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
void s_generate_metric (smart_str *metric, const char *sub_prefix, const char *name, double value, const char *unit, double sample_rate, zval *tags TSRMLS_DC)
{
    char *buffer;

    // First header, prefixes etc
    s_metric_header (metric, sub_prefix TSRMLS_CC);

    // Generate the metric
    spprintf (&buffer, 0, "%s:%f|%s|@%.3f", name, value, unit, sample_rate);
    smart_str_appends (metric, buffer);
    efree (buffer);

    // First append standard tags
    smart_str_appendc (metric, '|');

    // And footer
    s_metric_footer (metric, tags TSRMLS_CC);
}

static
zend_bool s_send_metric (const char *name, double value, const char *unit, double sample_rate, zval *tags TSRMLS_DC)
{
    zend_bool rc;
    smart_str metric = { 0 };

    if (!s_check_sample_rate (sample_rate TSRMLS_CC)) {
        return 1;
    }

    s_generate_metric (&metric, NULL, name, value, unit, sample_rate, tags TSRMLS_CC);

    rc = s_do_send (&metric TSRMLS_CC);
    smart_str_free (&metric);

    return rc;
}

static
void s_generate_incr_decr (smart_str *metric, const char *name, char sign, double value, const char *unit, double sample_rate, zval *tags TSRMLS_DC)
{
    char *buffer;

    // First header, prefixes etc
    s_metric_header (metric, NULL TSRMLS_CC);

    if (sign == '-')
        value *= -1.0;

    // Generate the metric
    spprintf (&buffer, 0, "%s:%+f|%s|@%.3f", name, value, unit, sample_rate);
    smart_str_appends (metric, buffer);
    efree (buffer);

    // First append standard tags
    smart_str_appendc (metric, '|');

    // And footer
    s_metric_footer (metric, tags TSRMLS_CC);
}

static
zend_bool s_send_incr_decr (const char *name, char sign, double value, const char *unit, double sample_rate, zval *tags TSRMLS_DC)
{
    zend_bool rc;
    smart_str metric = { 0 };

    if (!s_check_sample_rate (sample_rate TSRMLS_CC)) {
        return 1;
    }

    s_generate_incr_decr (&metric, name, sign, value, unit, sample_rate, tags TSRMLS_CC);

    rc = s_do_send (&metric TSRMLS_CC);
    smart_str_free (&metric);

    return rc;
}

static
void s_send_transaction (php_datadog_timing_t *timing, const char *sub_prefix, double sample_rate, zval *tags TSRMLS_DC)
{
    // End of request status
    struct timeval en_tv, real_tv;
    struct rusage  en_ru;
    smart_str tr_end = {0};

    // Check sampling rate
    if (!s_check_sample_rate (sample_rate TSRMLS_CC)) {
        return;
    }

    // Amount in milliseconds used
    if (gettimeofday (&en_tv, NULL) == 0) {
        timersub (&en_tv, &(timing->st_tv), &real_tv);

        s_generate_metric (&tr_end, sub_prefix, "time.real", timeval_to_msec (real_tv), "ms", sample_rate, tags TSRMLS_CC);
        smart_str_appendc (&tr_end, '\n');
    }

    // CPU usage
    if (getrusage (RUSAGE_SELF, &en_ru) == 0) {
        struct timeval tv_utime, tv_stime;

        timersub (&en_ru.ru_utime, &(timing->st_ru.ru_utime), &tv_utime);
        timersub (&en_ru.ru_stime, &(timing->st_ru.ru_stime), &tv_stime);

        s_generate_metric (&tr_end, sub_prefix, "cpu.user", timeval_to_double (tv_utime), "ms", sample_rate, tags TSRMLS_CC);
        smart_str_appendc (&tr_end, '\n');

        s_generate_metric (&tr_end, sub_prefix, "cpu.sys",  timeval_to_double (tv_stime), "ms", sample_rate, tags TSRMLS_CC);
        smart_str_appendc (&tr_end, '\n');

        s_generate_metric (&tr_end, sub_prefix, "hits",  1.0, "c", sample_rate, tags TSRMLS_CC);
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
    php_datadog_timing_t *timing = pemalloc (sizeof (php_datadog_timing_t), 1);

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
    double value;
    zval *tags = NULL;
    zend_bool retval;
    double sample_rate = 1.0;

    if (zend_parse_parameters (ZEND_NUM_ARGS() TSRMLS_CC, "sd|da!", &name, &name_len, &value, &sample_rate, &tags) != SUCCESS) {
        return;
    }

    if (!DATADOG_G (enabled)) {
        RETURN_FALSE;
    }

    retval = s_send_metric (name, value, type, sample_rate, tags TSRMLS_CC);
    RETVAL_BOOL (retval);
}

// Implementation of metrics
static
void s_datadog_incr_decr (INTERNAL_FUNCTION_PARAMETERS, char sign, const char *unit)
{
    char *name;
    int name_len;
    zval *tags = NULL;
    zend_bool retval;
    double value = 1.0, sample_rate = 1.0;

    if (zend_parse_parameters (ZEND_NUM_ARGS() TSRMLS_CC, "s|dda!", &name, &name_len, &value, &sample_rate, &tags) != SUCCESS) {
        return;
    }

    if (!DATADOG_G (enabled)) {
        RETURN_FALSE;
    }

    retval = s_send_incr_decr (name, sign, value, unit, sample_rate, tags TSRMLS_CC);
    RETVAL_BOOL (retval);
}

/* {{{ boolean datadog_timing(string $name, float $milliseconds[, float $sample_rate[, array $tags = array ()]])
    Create a timing for a specific entry
*/
PHP_FUNCTION(datadog_timing)
{
    s_datadog_metric_collection (INTERNAL_FUNCTION_PARAM_PASSTHRU, "ms");
}
/* }}} */

/* {{{ boolean datadog_gauge(string $name, float $value[, float $sample_rate[, array $tags = array ()]])
    Create a gauge for a specific entry
*/
PHP_FUNCTION(datadog_gauge)
{
    s_datadog_metric_collection (INTERNAL_FUNCTION_PARAM_PASSTHRU, "g");
}
/* }}} */

/* {{{ boolean datadog_histogram(string $name, float $value[, float $sample_rate[, array $tags = array ()]])
    Create a history for a specific entry
*/
PHP_FUNCTION(datadog_histogram)
{
    s_datadog_metric_collection (INTERNAL_FUNCTION_PARAM_PASSTHRU, "h");
}
/* }}} */

/* {{{ boolean datadog_set(string $name, float $value[, float $sample_rate[, array $tags = array ()]])
  
*/
PHP_FUNCTION(datadog_set)
{
    s_datadog_metric_collection (INTERNAL_FUNCTION_PARAM_PASSTHRU, "s");
}
/* }}} */

/* {{{ boolean datadog_counter_increment(string $name[, float $sample_rate[, array $tags = array ()]])
    Increment a counter
*/
PHP_FUNCTION(datadog_counter_increment)
{
    s_datadog_incr_decr (INTERNAL_FUNCTION_PARAM_PASSTHRU, '+', "c");
}
/* }}} */

/* {{{ boolean datadog_counter_decrement(string $name[, float $sample_rate[, array $tags = array ()]])
    Decrement a counter
*/
PHP_FUNCTION(datadog_counter_decrement)
{
    s_datadog_incr_decr (INTERNAL_FUNCTION_PARAM_PASSTHRU, '-', "c");
}
/* }}} */

/* {{{ boolean datadog_increment(string $name[, float $sample_rate[, array $tags = array ()]])
    Increment a counter
*/
PHP_FUNCTION(datadog_gauge_increment)
{
    s_datadog_incr_decr (INTERNAL_FUNCTION_PARAM_PASSTHRU, '+', "g");
}
/* }}} */

/* {{{ boolean datadog_decrement(string $name[, float $sample_rate[, array $tags = array ()]])
    Decrement a counter
*/
PHP_FUNCTION(datadog_gauge_decrement)
{
    s_datadog_incr_decr (INTERNAL_FUNCTION_PARAM_PASSTHRU, '-', "g");
}
/* }}} */

/* {{{ boolean datadog_transaction_begin(string $name[, float $sample_rate[, array $tags = array ()]])
    Create a timing for a specific transaction
*/
PHP_FUNCTION(datadog_transaction_begin)
{
    char *name;
    int name_len;
    double sample_rate = 1.0;
    zval *tags = NULL;

    if (zend_parse_parameters (ZEND_NUM_ARGS() TSRMLS_CC, "s|da!", &name, &name_len, &sample_rate, &tags) != SUCCESS) {
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
    // TODO: move sample_rate checking here so that we don't need to do this all on no match
    DATADOG_G (transaction)              = pemalloc (sizeof (php_datadog_transaction_t), 1);
    DATADOG_G (transaction)->timing      = s_datadog_timing (TSRMLS_C);
    DATADOG_G (transaction)->st_mem      = zend_memory_usage (1 TSRMLS_CC);
    DATADOG_G (transaction)->sample_rate = sample_rate;

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
        s_send_transaction (DATADOG_G (transaction)->timing, "transaction", DATADOG_G (transaction)->sample_rate, DATADOG_G (transaction)->tags TSRMLS_CC);

        memory_usage = (zend_memory_usage (1 TSRMLS_CC) - DATADOG_G (transaction)->st_mem);
        s_send_metric ("transaction.mem.used", memory_usage, "g", DATADOG_G (transaction)->sample_rate, DATADOG_G (transaction)->tags TSRMLS_CC);
    }

    Z_DELREF_P (DATADOG_G (transaction)->tags);
    zval_ptr_dtor (&(DATADOG_G (transaction)->tags));

    free (DATADOG_G (transaction)->timing);
    pefree (DATADOG_G (transaction), 1);

    DATADOG_G (transaction) = NULL;
    RETURN_TRUE;
}
/* }}} */

/* {{{ boolean datadog_set_application(string $name)
    Set application name, wrapper for ini_set ('datadog.application', $name);
*/
PHP_FUNCTION(datadog_set_application)
{
    char *name;
    int name_len;

    if (zend_parse_parameters (ZEND_NUM_ARGS() TSRMLS_CC, "s", &name, &name_len) != SUCCESS) {
        return;
    }

    if (!DATADOG_G (enabled)) {
        RETURN_FALSE;
    }

    // Update request tags
    if (zend_alter_ini_entry ("datadog.application", sizeof ("datadog.application"), name, name_len, PHP_INI_USER, PHP_INI_STAGE_RUNTIME) == FAILURE) {
        RETURN_FALSE;
    }
    RETURN_TRUE;
}
/* }}} */

/* {{{ boolean datadog_discard_request()
    Discards request statistics for this request. Useful for background tasks
*/
PHP_FUNCTION(datadog_discard_request)
{
    if (zend_parse_parameters_none () != SUCCESS) {
        return;
    }

    if (!DATADOG_G (enabled)) {
        RETURN_FALSE;
    }

    pefree (DATADOG_G (timing), 1);
    DATADOG_G (timing) = NULL;

    RETURN_TRUE;
}
/* }}} */

static
ZEND_INI_MH (OnUpdateDatadogErrorReporting)
{
    if (!new_value) {
        DATADOG_G (error_reporting) = E_ALL;
    } else {
        DATADOG_G (error_reporting) = atoi (new_value);
    }
    return SUCCESS;
}

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("datadog.enabled",           "1",                    PHP_INI_PERDIR, OnUpdateBool,                  enabled,           zend_datadog_globals, datadog_globals)
    STD_PHP_INI_ENTRY("datadog.agent",             "udp://127.0.0.1:8125", PHP_INI_PERDIR, OnUpdateString,                agent_addr,        zend_datadog_globals, datadog_globals)
    STD_PHP_INI_ENTRY("datadog.application",       "default",              PHP_INI_ALL,    OnUpdateString,                app_name,          zend_datadog_globals, datadog_globals)
    STD_PHP_INI_ENTRY("datadog.prefix",            "php.",                 PHP_INI_PERDIR, OnUpdateString,                prefix,            zend_datadog_globals, datadog_globals)
    STD_PHP_INI_ENTRY("datadog.strip_query",       "1",                    PHP_INI_PERDIR, OnUpdateBool,                  strip_query,       zend_datadog_globals, datadog_globals)
    STD_PHP_INI_ENTRY("datadog.error_reporting",   "E_ALL",                PHP_INI_ALL,    OnUpdateDatadogErrorReporting, error_reporting,   zend_datadog_globals, datadog_globals)
PHP_INI_END()

static
void s_datadog_capture_error (int type, const char *error_filename, const uint error_lineno, const char *format, va_list args)
{
    TSRMLS_FETCH ();

    if (DATADOG_G (enabled) && (DATADOG_G (error_reporting) & type) == type) {
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

        s_send_incr_decr ("error.reporting", '+', 1.0, "c", 1.0, tags TSRMLS_CC);
        zval_ptr_dtor (&tags);
    }
    // pass through to the original error callback
    orig_zend_error_cb (type, error_filename, error_lineno, format, args);
}

PHP_RINIT_FUNCTION(datadog)
{
    if (DATADOG_G (enabled)) {
        // Initialise request tags
        DATADOG_G (request_tags) = s_request_tags (TSRMLS_C);

        // Init datadog, main timer
        DATADOG_G (timing) = s_datadog_timing (TSRMLS_C);
    }
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(datadog)
{
    if (DATADOG_G (enabled)) {
        if (DATADOG_G (timing)) {
            long peak_memory = zend_memory_peak_usage (1 TSRMLS_CC);

            s_send_transaction (DATADOG_G (timing), "request", 1.0, NULL TSRMLS_CC);
            s_send_metric ("request.mem.peak", peak_memory, "g", 1.0, NULL TSRMLS_CC);
            pefree (DATADOG_G (timing), 1);
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
    orig_zend_error_cb = zend_error_cb;
    zend_error_cb = &s_datadog_capture_error;

    REGISTER_INI_ENTRIES();
    return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION(datadog) */
PHP_MSHUTDOWN_FUNCTION(datadog)
{
    zend_error_cb = orig_zend_error_cb;

    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}
/* }}} */

PHP_GINIT_FUNCTION(datadog)
{
    datadog_globals->background  = 0;
    datadog_globals->transaction = NULL;
}

/* {{{ PHP_MINFO_FUNCTION(datadog) */
PHP_MINFO_FUNCTION(datadog)
{
    php_info_print_table_start ();
    php_info_print_table_row (2, "datadog extension", "enabled");
    php_info_print_table_row (2, "datadog version",    PHP_DATADOG_EXTVER);
    php_info_print_table_end ();

    DISPLAY_INI_ENTRIES();
}

static zend_function_entry datadog_functions[] = {
    PHP_FE (datadog_timing,            NULL)
    PHP_FE (datadog_gauge,             NULL)
    PHP_FE (datadog_histogram,         NULL)
    PHP_FE (datadog_set,               NULL)
    PHP_FE (datadog_counter_increment, NULL)
    PHP_FE (datadog_counter_decrement, NULL)
    PHP_FE (datadog_gauge_increment,   NULL)
    PHP_FE (datadog_gauge_decrement,   NULL)
    PHP_FE (datadog_set_background,    NULL)
    PHP_FE (datadog_transaction_begin, NULL)
    PHP_FE (datadog_transaction_end,   NULL)
    PHP_FE (datadog_set_application,   NULL)
    PHP_FE (datadog_discard_request,   NULL)
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