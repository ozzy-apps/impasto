#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

class Command {
public:
    virtual ~Command() = default;
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual qint64 bytes() const { return 0; }
    // Lets rapid edits of the same thing (dragging an opacity slider) collapse
    // into one undo step. Return true after absorbing `next`.
    virtual bool mergeWith(const Command& next) { return false; }

    QString name;
};

class FnCommand : public Command {
public:
    FnCommand(const QString& n, std::function<void()> undoFn, std::function<void()> redoFn)
        : m_undo(std::move(undoFn)), m_redo(std::move(redoFn))
    {
        name = n;
    }
    void undo() override { m_undo(); }
    void redo() override { m_redo(); }

private:
    std::function<void()> m_undo, m_redo;
};

class CompoundCommand : public Command {
public:
    explicit CompoundCommand(const QString& n) { name = n; }
    void add(std::unique_ptr<Command> c) { m_children.push_back(std::move(c)); }
    void undo() override
    {
        for (auto it = m_children.rbegin(); it != m_children.rend(); ++it)
            (*it)->undo();
    }
    void redo() override
    {
        for (auto& c : m_children)
            c->redo();
    }
    qint64 bytes() const override
    {
        qint64 total = 0;
        for (auto& c : m_children)
            total += c->bytes();
        return total;
    }

private:
    std::vector<std::unique_ptr<Command>> m_children;
};

// Linear undo stack. Commands are pushed after their effect has been applied.
// Dropped commands may free GPU memory, so every call must happen with the
// engine's GL context current.
class History : public QObject {
    Q_OBJECT
public:
    explicit History(QObject* parent = nullptr);

    void push(std::unique_ptr<Command> cmd);
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    QString undo();
    QString redo();
    void clear();
    void breakMerge() { m_lastPush.invalidate(); }
    // Forgets the oldest step that can still be undone, and whatever it was
    // keeping. False when there is none to forget.
    bool dropOldest();

    int undoCount() const { return int(m_undo.size()); }
    // Changes whenever the stack does; lets callers tell "nothing has happened
    // since" apart from "same number of steps".
    quint64 serial() const { return m_serial; }
    qint64 bytes() const { return m_bytes; }

signals:
    void changed();

private:
    void trim();

    std::deque<std::unique_ptr<Command>> m_undo;
    std::deque<std::unique_ptr<Command>> m_redo;
    qint64 m_bytes = 0;
    quint64 m_serial = 0;
    int m_maxSteps = 250;
    qint64 m_maxBytes = qint64(1536) * 1024 * 1024;
    QElapsedTimer m_lastPush;
};
