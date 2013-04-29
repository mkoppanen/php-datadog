<?php

function run_in_transaction ($name, $function)
{
    if (extension_loaded ('datadog'))
        datadog_transaction_begin ($name);    

    $retval = $function ();

    if (extension_loaded ('datadog'))
        datadog_transaction_end ();    

    return $retval;
}
   
// Do some processing here
run_in_transaction ('test_transaction',
                     function () {
                         sleep (mt_rand (1, 3));
                     });
