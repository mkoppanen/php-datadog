--TEST--
Test transactions
--SKIPIF--
<?php require_once(dirname(__FILE__) . '/skipif.inc'); ?>
--FILE--
<?php
// Close and discard
datadog_transaction_begin ("hello");
    sleep (1);
datadog_transaction_end (true);

// Leave open
datadog_transaction_begin ("hello1");

echo "OK";
?>
--EXPECT--
OK