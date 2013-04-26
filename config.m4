PHP_ARG_ENABLE(datadog, whether to enable the datadog extension,
[  --enable-datadog      Enable PHP datadog extension])

if test "x$PHP_DATADOG" != "xno"; then
  PHP_NEW_EXTENSION(datadog, datadog.c, $ext_shared,,)
fi