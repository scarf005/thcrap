/**
  * Touhou Community Reliant Automatic Patcher
  * Main DLL
  *
  * ----
  *
  * Plug-in and module handling.
  */

#pragma once

/**
  * int thcrap_plugin_init(json_t *run_cfg)
  *
  * Parameters
  * ----------
  *	json_t *run_cfg
  *		Run configuration containing game, version, and user settings,
  *		merged in this order
  *
  * Return value
  * ------------
  *	0 on success
  *	1 if plugin loading failed and the plugin should be removed
  *
  * Called directly after the plugin was loaded via LoadLibrary.
  * To be identified as such, every thcrap plugin must export this function.
  */
typedef int (__stdcall *thcrap_plugin_init_type)(json_t *run_config);

/// Module functions
/// ================
/**
  * If the name of a function exported by any thcrap DLL matches the pattern
  * "*_mod_[suffix]", it is automatically executed when calling
  * mod_func_run() with [suffix]. The module hooks currently supported by
  * the thcrap core, with their parameter, include:
  *
  * • "init" (NULL)
  *   Called after a DLL has been loaded.
  *
  * • "detour" (NULL)
  *   Called after a DLL has been loaded. If a module needs to add any new
  *   detours, it should implement this hook, using one or more calls to
  *   detour_cache_add().
  *
  * • "repatch" (json_t *files_changed)
  *   Called when the given files have been changed outside the game and need
  *   to be reloaded. [files_changed] is a JSON object with the respective file
  *   names as keys.
  *
  * • "exit" (NULL)
  *   Called when shutting down the process.
  */

// Module function type.
typedef void (*mod_call_type)(void *param);

// Runs every exported function ending in "*_mod_[suffix]" across the
// functions in [funcs]. The order of execution is undefined.
void mod_func_run(json_t *funcs, const char *suffix, void *param);

// Calls mod_fun_run() with all registered functions from all thcrap DLLs.
void mod_func_run_all(const char *suffix, void *param);
/// ===================

// Initializes a plug-in DLL at [hMod]. This means registering all of its
// exports, and calling its "init" and "detour" module functions.
int plugin_init(HMODULE hMod);

// Loads all thcrap plugins from the current directory.
int plugins_load(void);
int plugins_close(void);
