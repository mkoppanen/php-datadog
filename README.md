Description
===========

PHP extension for sending metrics to http://datadoghq.com/

Build
=====

It is recommended to run:

    pear package
    
before building and using the generated package. This ensures that the version number is correctly replaced in the header files.


Automatic tags
==============

Currently the extension will add the following automatic tags to every metric:

    application:{datadog.application}
    filename:{basename PATH_TRANSLATED}


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

    // Increment a named metric
    boolean datadog_increment (string $name[, float $sample_rate, [array $tags = array ()]])

    // Decrement a named metric
    boolean datadog_decrement (string $name[, float $sample_rate, [array $tags = array ()]])

    // Begin a transaction, at the end of the transaction the following metrics are sent:
    // execution time, cpu usage (sys/user), memory usage
    boolean datadog_transaction_begin (string $name[, float $sample_rate, [array $tags = array ()]])

    // End a transaction
    boolean datadog_transaction_end ([boolean $discard = false])


INI settings
============

| Name                 | Default value          | Scope          | Description                                                    |
|----------------------|------------------------|----------------|----------------------------------------------------------------|
| datadog.enabled      | true                   | PHP_INI_PERDIR | Whether to enable datadog monitoring                           |
| datadog.agent        | "udp://127.0.0.1:8125" | PHP_INI_PERDIR | Address of the dd-agent                                        |
| datadog.application  | "default"              | PHP_INI_PERDIR | Application name to use in the automatic tag                   |
| datadog.prefix       | "php."                 | PHP_INI_PERDIR | Prefix to use for PHP metrics                                  |


Datadog extension monitors request times, request memory and CPU usage and rate of errors. Each PHP error will increment counter
called "error.reporting" (by default it will be prefixed with php.) and sets the level of the error as a tag for the metric.


Authors
=======

* Lentor Solutions http://lentor.io/


TODO
====

* Add support for sampling / sample rate
* Add support for sending events, currently pending on having non-blocking way to send events
* Compile on Windows
