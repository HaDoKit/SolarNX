#pragma once

#include <RmlUi/Core/FileInterface.h>
#include <SDL2/SDL.h>
#include <string>
#include <unordered_map>
#include <vector>

// RmlUi file access. Two namespaces:
//   "covers/<id>.<ext>" - in-memory cover images registered by the host (they arrive from the PC
//                       as encoded bytes, never touching storage)
//   everything else    - files under the asset root (SDL_RWops, so on Android they are read
//                        straight from the APK's assets/).
class ShellFileInterface : public Rml::FileInterface {
public:
    explicit ShellFileInterface(const char *asset_root);

    // Registers cover bytes under "covers/<id>.jpg"; call before the document references them.
    void SetCover(uint32_t id, const uint8_t *data, size_t size);
    void ClearCovers();

    Rml::FileHandle Open(const Rml::String &path) override;
    void Close(Rml::FileHandle file) override;
    size_t Read(void *buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;
    size_t Tell(Rml::FileHandle file) override;

private:
    struct Entry {
        SDL_RWops *rw;
        std::vector<uint8_t> owned; // keeps registered cover bytes alive while open
    };

    std::string root_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> covers_;
    std::unordered_map<Rml::FileHandle, Entry *> open_;
};
