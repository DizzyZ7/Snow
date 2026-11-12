#pragma once
#include <windows.h>
#include <memory>

namespace snow {
class Application {
public:
    Application(HINSTANCE instance, bool smokeTest, bool gpu, bool benchmark = false);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    int run();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
