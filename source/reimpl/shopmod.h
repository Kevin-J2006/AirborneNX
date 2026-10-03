#ifndef __REIMPL_SHOPMOD_H__
#define __REIMPL_SHOPMOD_H__

// The shop database with token-only vehicles also priced in credits (see
// shopmod.c). Given a translated path, returns the path the game should be
// served instead, or `path` itself.
const char *shopmod_redirect(const char *path);

// Hooks the game's JSON reader so that the price catalog it loads from
// initialfeed.dat gets the same credit prices. Call from so_patch, before
// the module is mapped.
struct so_module;
void shopmod_install(struct so_module *mod);

#endif // __REIMPL_SHOPMOD_H__
