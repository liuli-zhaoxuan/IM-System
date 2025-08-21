#pragma once
#include <string>

class FileCatalog
{
public:
    explicit FileCatalog(std::string root) : root_(std::move(root)) {}
    bool init(); // 确保目录存在（递归创建）

    const std::string &root() const { return root_; }
    std::string temp_path(const std::string &id) const;    // root/<id>.part
    std::string final_path(const std::string &name) const; // root/<name>

private:
    std::string root_;
};
