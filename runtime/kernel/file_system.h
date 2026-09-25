#pragma once
#include <filesystem>
#include <string>

// Monta um dispositivo do guest ("save:") numa pasta do host (XamContentCreateEx).
void MountDevice(const std::string& name, const std::filesystem::path& root, bool writable);
void UnmountDevice(const std::string& name);

// Pasta gravável do runtime (RAYMAN_DATA_DIR, padrão private/data).
std::filesystem::path DataDirectory();
