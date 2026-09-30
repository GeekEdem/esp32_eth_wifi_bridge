/* Berry module table for the firmware (see berry_conf.h for what is on). */
#include "berry.h"

be_extern_native_module(string);
be_extern_native_module(json);
be_extern_native_module(math);
be_extern_native_module(time);
be_extern_native_module(global);
be_extern_native_module(gc);
be_extern_native_module(introspect);
be_extern_native_module(strict);
be_extern_native_module(undefined);

BERRY_LOCAL const bntvmodule_t *const be_module_table[] = {
    &be_native_module(string),
    &be_native_module(json),
    &be_native_module(math),
    &be_native_module(time),
    &be_native_module(global),
    &be_native_module(gc),
    &be_native_module(introspect),
    &be_native_module(strict),
    &be_native_module(undefined),
    NULL
};

BERRY_LOCAL bclass_array be_class_table = {
    NULL
};
