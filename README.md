Description
===========

PHP extension for sending metrics to http://datadoghq.com/

Build
=====

It is recommended to run:

    pear package
    
before building and using the generated package. This ensures that the version number is correctly replaced in the header files.

Metrics per Request
===================

The following metrics are recorded for every request:

* {datadog.prefix}.request.time.real / ms
* {datadog.prefix}.request.cpu.user / ms
* {datadog.prefix}.request.cpu.sys / ms
* {datadog.prefix}.request.mem.peak / g
* {datadog.prefix}.request.hits / c

When ever a PHP error happens (E_PARSE, E_ERROR etc) the following counter is incremented:

* {datadog.prefix}.error.reporting

Automatic tags
==============

Currently the extension will add the following automatic tags to every metric:

    application:{datadog.application}
    filename:{basename PATH_TRANSLATED}
    request_uri:{REQUEST_URI} (see ini-setting datadog.strip_query)

The API
=======

    // Mark this run as background task. Automatically adds background:yes to all metrics
    boolean datadog_set_background (boolean $background)

    // Send a timing
    boolean datadog_timing (string $name, float $milliseconds[, float $sample_rate, [array $tags = array ()]])

    // Send a gauge
    boolean datadog_gauge (string $name, float $value[, float $sample_rate, [array $tags = array ()]])

    // Send a histogram
    boolean datadog_histogram (string $name, float $value[, float $sample_rate, [array $tags = array ()]])

    // Send a set
    boolean datadog_set (string $name, float $value[, float $sample_rate, [array $tags = array ()]])

    // Increment a named metric
    boolean datadog_increment (string $name[, float $sample_rate, [array $tags = array ()]])

    // Decrement a named metric
    boolean datadog_decrement (string $name[, float $sample_rate, [array $tags = array ()]])

    // Begin a transaction, at the end of the transaction the following metrics are sent:
    // execution time, cpu usage (sys/user), memory usage
    boolean datadog_transaction_begin (string $name[, float $sample_rate, [array $tags = array ()]])

    // End a transaction
    boolean datadog_transaction_end ([boolean $discard = false])

    // Sets the application name, affects metrics sent after the call
    boolean datadog_set_application (string $name)

    // Discard request statistics 
    boolean datadog_discard_request ()    
    

INI settings
============

| Name                     | Type      | Default value          | Scope          | Description                                                    |
|------------------------- |-----------|------------------------|----------------|----------------------------------------------------------------|
| datadog.enabled          | boolean   | true                   | PHP_INI_PERDIR | Whether to enable datadog monitoring                           |
| datadog.agent            | string    | "udp://127.0.0.1:8125" | PHP_INI_PERDIR | Address of the dd-agent                                        |
| datadog.application      | string    | "default"              | PHP_INI_ALL    | Application name to use in the automatic tag                   |
| datadog.prefix           | string    | "php."                 | PHP_INI_PERDIR | Prefix to use for PHP metrics                                  |
| datadog.strip_query      | boolean   | true                   | PHP_INI_PERDIR | Strip query string from request_uri tag                        |
| datadog.error_reporting  | integer   | E_ALL                  | PHP_INI_ALL    | Level of errors to report on the automatic error reporting     |

Datadog extension monitors request times, request memory and CPU usage and rate of errors. Each PHP error will increment counter
called "error.reporting" (by default it will be prefixed with php.) and sets the level of the error as a tag for the metric.


Authors
=======

* Lentor Solutions http://lentor.io/


TODO
====

* Add support for sending events, currently pending on having non-blocking way to send events
* Compile on Windows
