#include <platform/ZipArchive.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include <dirent.h>
#include <minizip/unzip.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shield::platform {
namespace {

constexpr std::size_t kZipBufferSize = 0x8000;

void EnsureDirectory(const char *path) {
    if((mkdir(path, 0777) != 0) && (errno != EEXIST)) {
        return;
    }
}

void EnsureParentDirectories(const std::string &path) {
    std::size_t separator = path.find('/');
    while(separator != std::string::npos) {
        const auto partial = path.substr(0, separator);
        if(!partial.empty() && (partial != "sdmc:")) {
            EnsureDirectory(partial.c_str());
        }
        separator = path.find('/', separator + 1);
    }
}

bool IsSafeRelativePath(const std::string &relative_path) {
    if(relative_path.empty()) {
        return false;
    }
    if(relative_path.find("..") != std::string::npos) {
        return false;
    }
    if((relative_path[0] == '/') || (relative_path[0] == '\\')) {
        return false;
    }
    return true;
}

std::string JoinPath(const std::string &root, const std::string &relative_path) {
    if(root.empty()) {
        return relative_path;
    }
    if(root.back() == '/') {
        return root + relative_path;
    }
    return root + "/" + relative_path;
}

bool ExtractCurrentFile(unzFile archive, const std::string &destination_path, const unz_file_info64 &file_info, std::string &error_message) {
    if(unzOpenCurrentFile(archive) != UNZ_OK) {
        error_message = "Failed to open zip entry";
        return false;
    }

    EnsureParentDirectories(destination_path);
    std::FILE *output = std::fopen(destination_path.c_str(), "wb");
    if(output == nullptr) {
        unzCloseCurrentFile(archive);
        error_message = "Failed to create extracted file";
        return false;
    }

    std::string buffer;
    buffer.resize(kZipBufferSize);
    std::uint64_t total_written = 0;

    while(total_written < file_info.uncompressed_size) {
        const int read_size = unzReadCurrentFile(archive, buffer.data(), static_cast<unsigned int>(buffer.size()));
        if(read_size < 0) {
            std::fclose(output);
            unzCloseCurrentFile(archive);
            error_message = "Failed to read zip entry";
            return false;
        }
        if(read_size == 0) {
            break;
        }

        const auto written = std::fwrite(buffer.data(), 1, static_cast<std::size_t>(read_size), output);
        if(written != static_cast<std::size_t>(read_size)) {
            std::fclose(output);
            unzCloseCurrentFile(archive);
            error_message = "Failed to write extracted file";
            return false;
        }

        total_written += static_cast<std::uint64_t>(written);
    }

    std::fflush(output);
    fsync(fileno(output));
    std::fclose(output);
    unzCloseCurrentFile(archive);

    if(total_written != file_info.uncompressed_size) {
        error_message = "Extracted file size mismatch";
        return false;
    }
    return true;
}

}

bool ZipArchive::ExtractAll(const std::string &archive_path, const std::string &destination_root, std::string &error_message) {
    error_message.clear();
    unzFile archive = unzOpen64(archive_path.c_str());
    if(archive == nullptr) {
        error_message = "Failed to open zip archive";
        return false;
    }

    int file_index = 0;
    for(;;) {
        const int code = (file_index == 0) ? unzGoToFirstFile(archive) : unzGoToNextFile(archive);
        if(code == UNZ_END_OF_LIST_OF_FILE) {
            break;
        }
        if(code != UNZ_OK) {
            unzClose(archive);
            error_message = "Failed to iterate zip archive";
            return false;
        }
        file_index++;

        unz_file_info64 file_info = {};
        if(unzGetCurrentFileInfo64(archive, &file_info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK) {
            unzClose(archive);
            error_message = "Failed to read zip entry info";
            return false;
        }

        std::string file_name;
        file_name.resize(static_cast<std::size_t>(file_info.size_filename));
        if(unzGetCurrentFileInfo64(archive, &file_info, file_name.data(), static_cast<unsigned long>(file_name.size()), nullptr, 0, nullptr, 0) != UNZ_OK) {
            unzClose(archive);
            error_message = "Failed to read zip entry name";
            return false;
        }

        if(!IsSafeRelativePath(file_name)) {
            unzClose(archive);
            error_message = "Unsafe zip entry path";
            return false;
        }

        const bool is_directory = !file_name.empty() && (file_name.back() == '/');
        const auto destination_path = JoinPath(destination_root, file_name);
        if(is_directory) {
            EnsureParentDirectories(destination_path);
            EnsureDirectory(destination_path.c_str());
            continue;
        }

        if(!ExtractCurrentFile(archive, destination_path, file_info, error_message)) {
            unzClose(archive);
            return false;
        }
    }

    unzClose(archive);
    return file_index > 0;
}

}
