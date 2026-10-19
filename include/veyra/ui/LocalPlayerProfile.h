#pragma once

#include <QString>
#include <memory>

namespace veyra::ui {

// A local video process never writes the browser resident's profile. Keep this
// owner alive until after its bridges/controllers have saved and been destroyed.
class LocalPlayerProfile final {
public:
    explicit LocalPlayerProfile(const QString& sharedDataDirectory);
    ~LocalPlayerProfile();

    LocalPlayerProfile(const LocalPlayerProfile&) = delete;
    LocalPlayerProfile& operator=(const LocalPlayerProfile&) = delete;
    LocalPlayerProfile(LocalPlayerProfile&&) = delete;
    LocalPlayerProfile& operator=(LocalPlayerProfile&&) = delete;

    bool ready() const;
    QString path() const;
    QString error() const;

    // Saves only allowed settings changed by this process. Unchanged settings
    // cannot undo another player's edits. For the same changed file, the last
    // commit wins. An error retains the private session for recovery.
    bool commit();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace veyra::ui
