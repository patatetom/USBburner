#include <cstdlib>
#include <iostream>
#include <string>

// Implementation of the missing MountUtilsLog function
void MountUtilsLog(std::string string) {
  const char* debug = std::getenv("MOUNTUTILS_DEBUG");
  if (debug != NULL) {
    std::cout << "[mountutils] " << string << std::endl;
  }
} 