#include <cpu.h>
#include <dirent.h>
#include <syscall.h>

int sys::dirent(int fd, struct dirent *ents, int off, int count) {
  if (ents != NULL && !curproc->mm->validate_pointer(ents, count * sizeof(struct dirent), VALIDATE_WRITE)) return -1;

  ck::ref<fs::File> file = curproc->get_fd(fd);

  if (file) {
    if (!file->ino->is_dir()) return -ENOTDIR;
    scoped_lock l = file->ino->lock();

    // TODO: this allocation is overkill and wasteful
    auto names = file->ino->dirents();
    if (ents == NULL) {
      return names.size();
    }

    if (off > names.size()) return -EINVAL;

    int c = 0;
    for (int i = off; i < count; i++) {
      // ents[i].ino = file->ino->ino;
      auto &n = names[i];
      int len = n->name.size();
      if (len >= 256) len = 255;


      ents[i].d_ino = 0;
      memcpy(ents[i].d_name, n->name.get(), len);
      c++;
    }

    return c;
  }

  return -1;
}
