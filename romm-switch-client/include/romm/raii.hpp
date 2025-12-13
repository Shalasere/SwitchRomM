#pragma once

#include <cstdio>
#include <utility>
#include <unistd.h>

namespace romm {

struct UniqueFd {
    int fd{-1};
    UniqueFd() = default;
    explicit UniqueFd(int f) : fd(f) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd(other.fd) { other.fd = -1; }
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }

    void reset(int newFd = -1) {
        if (fd >= 0) {
            ::close(fd);
        }
        fd = newFd;
    }

    explicit operator bool() const { return fd >= 0; }
};

struct UniqueFile {
    FILE* f{nullptr};
    UniqueFile() = default;
    explicit UniqueFile(FILE* f_) : f(f_) {}
    ~UniqueFile() { reset(); }

    UniqueFile(const UniqueFile&) = delete;
    UniqueFile& operator=(const UniqueFile&) = delete;

    UniqueFile(UniqueFile&& other) noexcept : f(other.f) { other.f = nullptr; }
    UniqueFile& operator=(UniqueFile&& other) noexcept {
        if (this != &other) {
            reset();
            f = other.f;
            other.f = nullptr;
        }
        return *this;
    }

    void reset(FILE* nf = nullptr) {
        if (f) {
            ::fclose(f);
        }
        f = nf;
    }

    explicit operator bool() const { return f != nullptr; }
};

template <class F>
class ScopeGuard {
public:
    explicit ScopeGuard(F&& fn) : fn_(std::forward<F>(fn)) {}
    ~ScopeGuard() { if (active_) fn_(); }
    void dismiss() { active_ = false; }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

    ScopeGuard(ScopeGuard&& other) noexcept
        : fn_(std::move(other.fn_)), active_(other.active_) {
        other.active_ = false;
    }
    ScopeGuard& operator=(ScopeGuard&& other) noexcept {
        if (this != &other) {
            if (active_) fn_();
            fn_ = std::move(other.fn_);
            active_ = other.active_;
            other.active_ = false;
        }
        return *this;
    }

private:
    F fn_;
    bool active_{true};
};

template <class F>
ScopeGuard<F> make_scope_guard(F&& fn) {
    return ScopeGuard<F>(std::forward<F>(fn));
}

} // namespace romm
