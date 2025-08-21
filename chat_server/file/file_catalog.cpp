#include "file/file_catalog.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>

static bool ensure_dir(const std::string &dir)
{
    struct stat st{};
    if (::stat(dir.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode);
    // 递归创建
    auto pos = dir.find_last_of('/');
    if (pos != std::string::npos)
    {
        std::string parent = dir.substr(0, pos);
        if (!parent.empty() && !ensure_dir(parent))
            return false;
    }
    return ::mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
}

bool FileCatalog::init() { return ensure_dir(root_); }
std::string FileCatalog::temp_path(const std::string &id) const { return root_ + "/" + id + ".part"; }
std::string FileCatalog::final_path(const std::string &name) const { return root_ + "/" + name; }
