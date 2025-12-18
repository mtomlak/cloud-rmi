PHP_ARG_WITH(profiweb_cloudrmi, for CloudRMI support,
[  --with-profiweb_cloudrmi[=DIR]    Include ProfiWeb CloudRMI support], yes, no)

if test "$PHP_PROFIWEB_CLOUDRMI" != "no"; then
  if test "$PHP_PROFIWEB_CLOUDRMI" = "yes"; then
    PHP_PROFIWEB_CLOUDRMI=$(pwd)/../../libcloudrmi
  fi

  if test -f "$PHP_PROFIWEB_CLOUDRMI/include/cloudrmi/protocol.h"; then
    PHP_ADD_INCLUDE($PHP_PROFIWEB_CLOUDRMI/include)
  else
    AC_MSG_ERROR([cloudrmi headers not found in $PHP_PROFIWEB_CLOUDRMI/include])
  fi

  if test -f "$PHP_PROFIWEB_CLOUDRMI/libcloudrmi.a"; then
    PROFIWEB_CLOUDRMI_SHARED_LIBADD="-L$PHP_PROFIWEB_CLOUDRMI -lcloudrmi"
  elif test -f "$PHP_PROFIWEB_CLOUDRMI/lib/libcloudrmi.a"; then
    PROFIWEB_CLOUDRMI_SHARED_LIBADD="-L$PHP_PROFIWEB_CLOUDRMI/lib -lcloudrmi"
  else
    AC_MSG_ERROR([cloudrmi library not found (expected libcloudrmi.a)])
  fi

  PHP_SUBST(PROFIWEB_CLOUDRMI_SHARED_LIBADD)
  PHP_NEW_EXTENSION(profiweb_cloudrmi, cloudrmi.c classes.c, yes)
fi
