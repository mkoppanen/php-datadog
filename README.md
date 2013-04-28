PHP extension for sending metrics to http://datadoghq.com/

Automatic tags
==============

Currently the extension will add the following automatic tags to every metric:

    application:{datadog.application}
    filename:{basename PATH_TRANSLATED}

The API
=======

    boolean datadog_set_background (boolean $background)

    boolean datadog_timing (string $name, int $milliseconds[, array $tags = array ()])

    boolean datadog_gauge (string $name, int $value[, array $tags = array ()])

    boolean datadog_histogram (string $name, int $value[, array $tags = array ()])

    boolean datadog_increment (string $name[, array $tags = array ()])

    boolean datadog_decrement (string $name[, array $tags = array ()])

    boolean datadog_transaction_begin (string $name[, array $tags = array ()])

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
