PHP extension for sending metrics to http://datadoghq.com/


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

    | Name                 | Default value          | Scope          |
    |----------------------|------------------------|----------------|
    |"datadog.enabled"     | "1"                    | PHP_INI_PERDIR |
    |"datadog.agent"       | "udp://127.0.0.1:8125" | PHP_INI_PERDIR |
    |"datadog.application" | "default"              | PHP_INI_PERDIR |
    |"datadog.prefix"      | "php."                 | PHP_INI_PERDIR |