#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sys {

struct Toast {
    std::wstring app, aumid, title, body, launch, image;
    std::vector<uint32_t> iconPx; // premultiplied BGRA app/sender icon, read off the ui thread
    unsigned iconW = 0, iconH = 0;
    int iconGlyph = 0; // optional fallback glyph when no app icon bitmap is provided (e.g. timer alerts)
    bool iconRound = false, silent = false;
};

class NotifyWatch { // mirrors new windows notifications; holds nothing unless started
public:
    NotifyWatch();
    ~NotifyWatch();
    NotifyWatch(const NotifyWatch&) = delete;
    NotifyWatch& operator=(const NotifyWatch&) = delete;
    void Start(HWND notify, UINT msg); // ui thread: asks for access if needed, then listens for toast popups
    void Stop();
    bool Pop(Toast& out); // oldest waiting notification, posted msg means one is waiting
    bool More() const; // another one is waiting after this

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
