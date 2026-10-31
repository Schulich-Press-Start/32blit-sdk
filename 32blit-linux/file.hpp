/**
 * \file file.hpp
 * \brief 32blit file API implemented with POSIX calls.
 *
 * Paths passed in by the engine are relative to the game's directory
 * (the folder holding the executable), the same rule the SDL port uses.
 * The one exception is the save directory: `get_save_path()` returns an
 * absolute path, and any path that starts with it is used unchanged.
 *
 * Layout on disk:
 * \code
 * <game dir>/<game executable>
 * <game dir>/<asset files the game opens>
 * <game dir>/saves/<game title>/save0 ...
 * \endcode
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "engine/file.hpp"

namespace blit_linux {

  /**
   * \brief Work out the game and save directories, and create the save directory.
   *
   * Must be called before the game's `init()`.
   *
   * \param title Game title from metadata, used as the save folder name.
   */
  void file_init(const char *title);

  /**
   * \brief Open a file.
   * \param file Path relative to the game directory, or an absolute path inside the save directory.
   * \param mode `blit::OpenMode::read`, `write` (create/truncate) or `read | write` (must exist).
   * \return Opaque handle for the other calls, or `nullptr` on failure.
   */
  void *open_file(const std::string &file, int mode);

  /**
   * \brief Read from an open file at an absolute offset.
   * \return Bytes read (less than `length` at end of file), or -1 on error.
   */
  int32_t read_file(void *fh, uint32_t offset, uint32_t length, char *buffer);

  /**
   * \brief Write to an open file at an absolute offset.
   * \return Bytes written, or -1 on error.
   */
  int32_t write_file(void *fh, uint32_t offset, uint32_t length, const char *buffer);

  /**
   * \brief Close a handle from open_file() and free it.
   * \return 0 on success, -1 on error.
   */
  int32_t close_file(void *fh);

  /** \brief Size of an open file in bytes. */
  uint32_t get_file_length(void *fh);

  /**
   * \brief Call `callback` once per entry in a directory (skips `.` and `..`).
   * \param path Directory relative to the game directory.
   */
  void list_files(const std::string &path, std::function<void(blit::FileInfo &)> callback);

  /** \brief True if `path` exists and is a regular file. */
  bool file_exists(const std::string &path);

  /** \brief True if `path` exists and is a directory. */
  bool directory_exists(const std::string &path);

  /** \brief Create one directory level. Succeeds if it already exists. */
  bool create_directory(const std::string &path);

  /** \brief Rename or move a file. */
  bool rename_file(const std::string &old_name, const std::string &new_name);

  /** \brief Delete a file or an empty directory. */
  bool remove_file(const std::string &path);

  /** \brief Absolute path of this game's save directory, ending in `/`. */
  const char *get_save_path();

  /** \brief True if the save directory exists and is writable. */
  bool is_storage_available();
}
