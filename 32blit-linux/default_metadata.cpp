/**
 * \file default_metadata.cpp
 * \brief Fallback metadata for games built without `blit_metadata()`.
 *
 * Each symbol is weak, so the strings generated from a game's
 * `metadata.yml` replace these at link time. Keep nothing else in this
 * file so the linker can drop it entirely when it isn't needed.
 */

[[gnu::weak]] const char *metadata_title = "32Blit Game";
[[gnu::weak]] const char *metadata_author = "Unknown";
[[gnu::weak]] const char *metadata_description = "";
[[gnu::weak]] const char *metadata_version = "v0.0.0";
[[gnu::weak]] const char *metadata_url = "";
[[gnu::weak]] const char *metadata_category = "";
