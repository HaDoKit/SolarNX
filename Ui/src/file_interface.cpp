#include "file_interface.h"

#include <cstdlib>

ShellFileInterface::ShellFileInterface(const char *asset_root)
    : root_(asset_root != NULL ? asset_root : "")
{
    if (!root_.empty() && root_.back() != '/')
        root_ += '/';
}

void ShellFileInterface::SetCover(uint32_t id, const uint8_t *data, size_t size)
{
    if (data == NULL || size == 0)
    {
        covers_.erase(id);
        return;
    }
    covers_[id] = std::vector<uint8_t>(data, data + size);
}

void ShellFileInterface::ClearCovers()
{
    covers_.clear();
}

Rml::FileHandle ShellFileInterface::Open(const Rml::String &path)
{
    SDL_RWops *rw = NULL;
    Entry *entry = NULL;

    if (path.compare(0, 7, "covers/") == 0)
    {
        // "covers/<id>.<ext>"
        uint32_t id = (uint32_t)strtoul(path.c_str() + 7, NULL, 10);
        auto it = covers_.find(id);
        if (it == covers_.end())
            return (Rml::FileHandle)0;
        entry = new Entry();
        entry->owned = it->second; // snapshot so set_library can't pull the rug mid-read
        rw = SDL_RWFromConstMem(entry->owned.data(), (int)entry->owned.size());
    }
    else
    {
        rw = SDL_RWFromFile((root_ + path).c_str(), "rb");
    }

    if (rw == NULL)
    {
        delete entry;
        return (Rml::FileHandle)0;
    }
    if (entry == NULL)
        entry = new Entry();
    entry->rw = rw;
    Rml::FileHandle handle = (Rml::FileHandle)entry;
    open_[handle] = entry;
    return handle;
}

void ShellFileInterface::Close(Rml::FileHandle file)
{
    auto it = open_.find(file);
    if (it == open_.end())
        return;
    SDL_RWclose(it->second->rw);
    delete it->second;
    open_.erase(it);
}

size_t ShellFileInterface::Read(void *buffer, size_t size, Rml::FileHandle file)
{
    auto it = open_.find(file);
    if (it == open_.end())
        return 0;
    return SDL_RWread(it->second->rw, buffer, 1, size);
}

bool ShellFileInterface::Seek(Rml::FileHandle file, long offset, int origin)
{
    auto it = open_.find(file);
    if (it == open_.end())
        return false;
    int whence = origin == SEEK_SET ? RW_SEEK_SET : origin == SEEK_END ? RW_SEEK_END : RW_SEEK_CUR;
    return SDL_RWseek(it->second->rw, offset, whence) >= 0;
}

size_t ShellFileInterface::Tell(Rml::FileHandle file)
{
    auto it = open_.find(file);
    if (it == open_.end())
        return 0;
    return (size_t)SDL_RWtell(it->second->rw);
}
