#pragma once

// Instala handlers de SIGSEGV/SIGBUS/SIGILL/SIGTRAP/... que imprimem o endereço
// do guest, a função recompilada e os registradores, e saem na hora.
void InstallCrashHandler();
