#ifndef SCOURGEBRINGER_SAVE_MIGRATION_H
#define SCOURGEBRINGER_SAVE_MIGRATION_H

/*
 * Prepare the writable Android app directories and copy saves left by builds
 * that incorrectly exposed the immutable library directory as filesDir.
 * Existing destination files always win and legacy files are never removed.
 */
int sb_prepare_data_dirs(const char *data_dir);
int sb_migrate_legacy_saves(const char *game_dir, const char *data_dir);

#endif
