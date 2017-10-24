﻿/**
  * Touhou Community Reliant Automatic Patcher
  * Tasogare Frontier support plugin
  *
  * ----
  *
  * Plugin entry point and breakpoints.
  */

#include <thcrap.h>
#include <vfs.h>
#include "thcrap_tasofro.h"
#include "pl.h"
#include "tfcs.h"
#include "act-nut.h"
#include "spellcards_generator.h"
#include "bgm.h"
#include "plugin.h"
#include "crypt.h"

tasofro_game_t game_id;

// Translate strings to IDs.
static tasofro_game_t game_id_from_string(const char *game)
{
	if (game == NULL) {
		return TH_NONE;
	}
	else if (!strcmp(game, "th135")) {
		return TH135;
	}
	else if (!strcmp(game, "th145")) {
		return TH145;
	}
	return TH_FUTURE;
}

// TODO: read the file names list in JSON format
int __stdcall thcrap_plugin_init()
{
	char* filenames_list;
	size_t filenames_list_size;

	int base_tasofro_removed = stack_remove_if_unneeded("base_tasofro");
	if (base_tasofro_removed == 1) {
		return 1;
	}

	const char *game = json_object_get_string(runconfig_get(), "game");
	game_id = game_id_from_string(game);

	if(base_tasofro_removed == -1) {
		if(game_id == TH145) {
			log_mboxf(NULL, MB_OK | MB_ICONINFORMATION,
				"Support for TH14.5 has been moved out of the sandbox.\n"
				"\n"
				"Please reconfigure your patch stack; otherwise, you might "
				"encounter errors or missing functionality after further "
				"updates.\n"
			);
		} else {
			return 1;
		}
	}

	const char *crypt = json_object_get_string(runconfig_get(), "crypt");
	if (game_id >= TH145) {
		ICrypt::instance = new CryptTh145();
	}
	else {
		ICrypt::instance = new CryptTh135();
	}
	
	patchhook_register("*/stage*.pl", patch_pl);
	patchhook_register("*/ed_*.pl", patch_pl);
	patchhook_register("*.csv", patch_tfcs);
	patchhook_register("*.dll", patch_dll);
	patchhook_register("*.act", patch_act);
	patchhook_register("*.nut", patch_nut);
	patchhook_register("*.txt", patch_plaintext);

	jsonvfs_game_add("data/csv/story/*/stage*.csv.jdiff",						{ "spells.js" }, spell_story_generator);
	if (game_id >= TH145) {
		jsonvfs_game_add("data/csv/spellcard/*.csv.jdiff",						{ "spells.js" }, spell_player_generator);
		jsonvfs_game_add("data/system/char_select3/*/equip/*/000.png.csv.jdiff",{ "spells.js" }, spell_char_select_generator);
	}
	else {
		jsonvfs_game_add("data/csv/Item*.csv.jdiff",							{ "spells.js" }, spell_player_generator);
	}

	char *bgm_pattern_fn = fn_for_game("data/bgm/bgm.csv.jdiff");
	char *musiccmt_fn = fn_for_game("musiccmt.js");
	jsonvfs_add(bgm_pattern_fn, { "themes.js", musiccmt_fn }, bgm_generator);
	SAFE_FREE(musiccmt_fn);
	SAFE_FREE(bgm_pattern_fn);

	filenames_list = (char*)stack_game_file_resolve("fileslist.txt", &filenames_list_size);
	LoadFileNameListFromMemory(filenames_list, filenames_list_size);
	return 0;
}


file_rep_t *call_file_header(x86_reg_t *regs, json_t *bp_info, const char *filename)
{
	json_t *new_bp_info = json_copy(bp_info);
	json_object_set_new(new_bp_info, "file_name", json_integer((json_int_t)filename));
	BP_file_header(regs, new_bp_info);
	json_decref(new_bp_info);

	file_rep_t *fr = file_rep_get(filename);
	if (fr && (fr->rep_buffer || fr->patch)) {
		return fr;
	}
	return nullptr;
}

int BP_th135_file_header(x86_reg_t *regs, json_t *bp_info)
{
	// Parameters
	// ----------
	DWORD hash = json_object_get_immediate(bp_info, regs, "file_hash");
	DWORD *key = (DWORD*)json_object_get_immediate(bp_info, regs, "file_key");
	// ----------

	if (!hash || !key)
		return 1;

	struct FileHeader *header = hash_to_file_header(hash);
	if (!header) {
		return 1;
	}

	file_rep_t *fr = call_file_header(regs, bp_info, header->path);

	// If the game loads a DDS file and we have no corresponding DDS file, try to replace it with a PNG file (the game will deal with it)
	if (!fr && strcmp(PathFindExtensionA(header->path), ".dds") == 0) {
		char png_path[MAX_PATH];
		strcpy(png_path, header->path);
		strcpy(PathFindExtensionA(png_path), ".png");
		register_filename(png_path);
		file_rep_t *fr_png = call_file_header(regs, bp_info, png_path);

		if (fr_png) {
			fr = file_rep_get(header->path);
			file_rep_clear(fr);
			memcpy(fr, fr_png, sizeof(*fr));
			memset(fr_png, 0, sizeof(*fr_png));
			file_rep_clear(fr_png);
			SAFE_FREE(fr->name);
			fr->name = EnsureUTF8(header->path, strlen(header->path) + 1);
		}
		else {
			fr = nullptr;
		}
	}

	if (fr) {
		header->hash = hash;
		memcpy(header->key, key, 4 * sizeof(DWORD));
		ICrypt::instance->convertKey(header->key);
		fr->object = header;
	}
	header->fr = fr;

	return 1;
}

static void post_read(const file_rep_t *fr, BYTE *buffer, size_t size)
{
	const FileHeader *header = (const FileHeader*)fr->object;
	if (header) {
		ICrypt::instance->uncryptBlock(buffer, size, header->key);
	}
}

static void post_patch(const file_rep_t *fr, BYTE *buffer, size_t size)
{
	const FileHeader *header = (const FileHeader*)fr->object;
	if (header) {
		ICrypt::instance->cryptBlock(buffer, size, header->key);
	}
}

/**
  * Replace file, 3rd attempt - hopefully I won't have to write a 4th one.
  * This breakpoint takes care of patching the files.
  * It takes place in the game's file reader, right after it calls its ReadFile caller.
  * At this point, we have the game's file structure in ESI, and so the filename's hash - we can't
  * read the wrong file.
  * In th145, the game's file structure have the following fields:
  * void* vtable;
  * HANDLE hFile;
  * BYTE buffer[0x10000];
  * DWORD LastReadFileSize; // Last value returned in lpNumberOfBytesRead in ReadFile
  * DWORD Offset; // Offset for the reader. I don't know what happens to it when it's bigger than 0x10000
  * DWORD LastReadSize; // Last read size asked to the reader
  * DWORD unknown1;
  * DWORD FileSize;
  * DWORD FileNameHash;
  * DWORD unknown2;
  * DWORD XorOffset; // Offset used by the XOR function
  * DWORD Key[5]; // 32-bytes encryption key used for XORing. The 5th DWORD is a copy of the 1st DWORD.
  * DWORD Aux; // Contains the last DWORD read from the file. Used during XORing.
  * We use hFile (file_struct + 4), buffer (file_struct + 8) and FileNameHash (file_struct + 0x1001c).
  * All the other fields are listed for documentation.
  */
int BP_th135_read_file(x86_reg_t *regs, json_t *bp_info)
{
	// Parameters
	// ----------
	DWORD newHash = json_object_get_immediate(bp_info, regs, "hash");
	// ----------

	static DWORD hash = 0;

	if (newHash) {
		hash = newHash;
	}
	if (!hash) {
		return 1;
	}

	struct FileHeader *header = hash_to_file_header(hash);
	if (!header || !header->fr || (!header->fr->rep_buffer && !header->fr->patch)) {
		// Nothing to patch.
		return 1;
	}

	json_t *new_bp_info = json_copy(bp_info);
	json_object_set_new(new_bp_info, "file_name",  json_integer((json_int_t)header->fr->name));
	json_object_set_new(new_bp_info, "post_read",  json_integer((json_int_t)post_read));
	json_object_set_new(new_bp_info, "post_patch", json_integer((json_int_t)post_patch));
	int ret = BP_fragmented_read_file(regs, new_bp_info);
	json_decref(new_bp_info);
	return ret;
}
