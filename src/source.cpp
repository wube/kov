#include "source.hpp"
#include <algorithm>
#include <cstdio>

std::vector<std::filesystem::path> collectFiles(const std::filesystem::path& root,
                                                const std::vector<std::string>& extensions)
{
  std::vector<std::filesystem::path> result;
  if (std::filesystem::is_regular_file(root))
  {
    result.push_back(root);
    return result;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
         root, std::filesystem::directory_options::skip_permission_denied))
  {
    if (!entry.is_regular_file())
      continue;
    std::string extension = entry.path().extension().string();
    if (std::find(extensions.begin(), extensions.end(), extension) != extensions.end())
      result.push_back(entry.path());
  }
  std::sort(result.begin(), result.end());
  return result;
}

bool readFile(const std::filesystem::path& path, std::string& out)
{
  FILE* file = _wfopen(path.c_str(), L"rb");
  if (!file)
    return false;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  out.resize(size_t(size));
  size_t read = fread(out.data(), 1, size_t(size), file);
  fclose(file);
  out.resize(read);
  out.reserve(read + 1); // std::string guarantees the trailing '\0' sentinel.
  return true;
}
