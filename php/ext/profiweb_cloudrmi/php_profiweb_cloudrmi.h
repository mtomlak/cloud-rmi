#ifndef PHP_PROFIWEB_CLOUDRMI_H
#define PHP_PROFIWEB_CLOUDRMI_H

extern zend_module_entry cloudrmi_module_entry;
#define phpext_profiweb_cloudrmi_ptr &cloudrmi_module_entry

#define PHP_PROFIWEB_CLOUDRMI_VERSION "0.1.0"

void cloudrmi_register_classes(INIT_FUNC_ARGS);

#endif /* PHP_PROFIWEB_CLOUDRMI_H */
