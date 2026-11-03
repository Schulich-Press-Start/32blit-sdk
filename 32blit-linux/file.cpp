/**
 * \file file.cpp
 * \brief POSIX implementation of the 32blit file API.
 *
 * Based on the non-Windows branch of `32blit-sdl/File.cpp`, with the SDL
 * calls replaced by plain file descriptors. Reads and writes use
 * `pread()`/`pwrite()` because the 32blit API always passes an absolute
 * offset, so there is no shared file position to keep in sync.
 */
#include "file.hpp"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace blit_linux {

  namespace {
    /// Directory holding the game executable, ending in `/`. Relative paths are resolved against it.
    std::string base_path;

    /// This game's save directory (absolute), ending in `/`.
    std::string save_path;

    /// What a `void *` file handle points to.
    struct FileHandle {
      /// Open file descriptor.
      int fd;
    };

    /**
     * \brief Turn an engine path into a real filesystem path.
     *
     * Paths inside the save directory are already absolute; everything else
     * is relative to the game directory.
     */
    std::string map_path(const std::string &path) {
      if(!save_path.empty() && path.compare(0, save_path.length(), save_path) == 0)
        return path;

      return base_path + path;
    }

    /** \brief Directory containing the running executable, ending in `/`. */
    std::string executable_directory() {
      char buf[PATH_MAX];
      ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);

      if(len <= 0)
        return "./";

      buf[len] = '\0';
      std::string path(buf);
      return path.substr(0, path.find_last_of('/') + 1);
    }

    /**
     * \brief Make a game title safe to use as a folder name.
     *
     * Replaces `/` (the only character Linux forbids besides NUL) and stops
     * titles like `..` from escaping the saves folder.
     */
    std::string folder_name(const char *title) {
      std::string name = (title && title[0]) ? title : "game";

      for(auto &c : name) {
        if(c == '/')
          c = '_';
      }

      if(name == "." || name == "..")
        name = "game";

      return name;
    }

    /**
     * \brief Create a directory and any missing parents (`mkdir -p`).
     * \return true if the full path exists as a directory afterwards.
     */
    bool make_directories(const std::string &path) {
      size_t pos = 0;

      do {
        pos = path.find('/', pos + 1);
        auto part = path.substr(0, pos);

        if(mkdir(part.c_str(), 0755) != 0 && errno != EEXIST)
          return false;
      } while(pos != std::string::npos && pos + 1 < path.length());

      struct stat st;
      return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }
  }

  void file_init(const char *title) {
    base_path = executable_directory();
    save_path = base_path + "saves/" + folder_name(title) + "/";

    if(!make_directories(save_path))
      fprintf(stderr, "32blit-linux: cannot create save directory %s: %s\n", save_path.c_str(), strerror(errno));
  }

  void *open_file(const std::string &file, int mode) {
    int flags;

    if(mode == blit::OpenMode::read)
      flags = O_RDONLY;
    else if(mode == blit::OpenMode::write)
      flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if(mode == (blit::OpenMode::read | blit::OpenMode::write))
      // like fopen "r+": the file must already exist
      flags = O_RDWR;
    else
      return nullptr;

    int fd = open(map_path(file).c_str(), flags | O_CLOEXEC, 0644);

    if(fd < 0)
      return nullptr;

    // open() happily opens directories read-only; the engine expects files only
    struct stat st;
    if(fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
      close(fd);
      return nullptr;
    }

    return new FileHandle{fd};
  }

  int32_t read_file(void *fh, uint32_t offset, uint32_t length, char *buffer) {
    if(!fh)
      return -1;

    auto fd = static_cast<FileHandle *>(fh)->fd;
    uint32_t total = 0;

    // pread can return fewer bytes than asked for; keep going until done or end of file
    while(total < length) {
      ssize_t got = pread(fd, buffer + total, length - total, off_t(offset) + total);

      if(got < 0) {
        if(errno == EINTR)
          continue;
        return -1;
      }

      // end of file
      if(got == 0)
        break;

      total += uint32_t(got);
    }

    return int32_t(total);
  }

  int32_t write_file(void *fh, uint32_t offset, uint32_t length, const char *buffer) {
    if(!fh)
      return -1;

    auto fd = static_cast<FileHandle *>(fh)->fd;
    uint32_t total = 0;

    while(total < length) {
      ssize_t put = pwrite(fd, buffer + total, length - total, off_t(offset) + total);

      if(put < 0) {
        if(errno == EINTR)
          continue;
        return -1;
      }

      total += uint32_t(put);
    }

    return int32_t(total);
  }

  int32_t close_file(void *fh) {
    if(!fh)
      return -1;

    auto handle = static_cast<FileHandle *>(fh);
    int ret = close(handle->fd);
    delete handle;

    return ret == 0 ? 0 : -1;
  }

  uint32_t get_file_length(void *fh) {
    struct stat st;

    if(!fh || fstat(static_cast<FileHandle *>(fh)->fd, &st) != 0)
      return 0;

    return uint32_t(st.st_size);
  }

  void list_files(const std::string &path, std::function<void(blit::FileInfo &)> callback) {
    auto mapped_path = map_path(path);
    auto dir = opendir(mapped_path.c_str());

    if(!dir)
      return;

    while(auto ent = readdir(dir)) {
      blit::FileInfo info;
      info.name = ent->d_name;

      if(info.name == "." || info.name == "..")
        continue;

      // stat follows symlinks, so a link to a folder is listed as a folder
      struct stat st;
      if(stat((mapped_path + "/" + info.name).c_str(), &st) != 0)
        continue;

      info.flags = S_ISDIR(st.st_mode) ? blit::FileFlags::directory : 0;
      info.size = uint32_t(st.st_size);

      callback(info);
    }

    closedir(dir);
  }

  bool file_exists(const std::string &path) {
    struct stat st;
    return stat(map_path(path).c_str(), &st) == 0 && S_ISREG(st.st_mode);
  }

  bool directory_exists(const std::string &path) {
    struct stat st;
    return stat(map_path(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  }

  bool create_directory(const std::string &path) {
    return mkdir(map_path(path).c_str(), 0755) == 0 || errno == EEXIST;
  }

  bool rename_file(const std::string &old_name, const std::string &new_name) {
    return rename(map_path(old_name).c_str(), map_path(new_name).c_str()) == 0;
  }

  bool remove_file(const std::string &path) {
    return remove(map_path(path).c_str()) == 0;
  }

  const char *get_save_path() {
    return save_path.c_str();
  }

  bool is_storage_available() {
    return access(save_path.c_str(), W_OK) == 0;
  }
}
