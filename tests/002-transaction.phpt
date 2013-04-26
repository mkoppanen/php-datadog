--TEST--
Test transactions
--SKIPIF--
<?php require_once(dirname(__FILE__) . '/skipif.inc'); ?>
--FILE--
<?php
datadog_transaction_begin ("hello");
    sleep (1);
datadog_transaction_end (true);
echo "OK";
?>
--EXPECT--
OK