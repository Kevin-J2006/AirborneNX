#ifndef __DYNLIB_H__
#define __DYNLIB_H__

#include <so_util/so_util.h>

void init_android_streams(void);
void resolve_dynamic_dependencies(so_module *mod);

#endif // __DYNLIB_H__
