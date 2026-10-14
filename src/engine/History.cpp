#include "engine/History.h"

History::History(QObject* parent) : QObject(parent) {}

void History::push(std::unique_ptr<Command> cmd)
{
    for (auto& c : m_redo)
        m_bytes -= c->bytes();
    m_redo.clear();

    const bool recent = m_lastPush.isValid() && m_lastPush.elapsed() < 900;
    m_lastPush.start();
    if (recent && !m_undo.empty() && m_undo.back()->mergeWith(*cmd)) {
        ++m_serial;
        emit changed();
        return;
    }
    m_bytes += cmd->bytes();
    m_undo.push_back(std::move(cmd));
    trim();
    ++m_serial;
    emit changed();
}

void History::trim()
{
    while (m_undo.size() > 1 && (int(m_undo.size()) > m_maxSteps || m_bytes > m_maxBytes)) {
        m_bytes -= m_undo.front()->bytes();
        m_undo.pop_front();
    }
}

QString History::undo()
{
    if (m_undo.empty())
        return {};
    m_lastPush.invalidate();
    std::unique_ptr<Command> cmd = std::move(m_undo.back());
    m_undo.pop_back();
    // What a command keeps can change size as it trades places with the document.
    const qint64 was = cmd->bytes();
    cmd->undo();
    m_bytes += cmd->bytes() - was;
    const QString name = cmd->name;
    m_redo.push_back(std::move(cmd));
    ++m_serial;
    emit changed();
    return name;
}

QString History::redo()
{
    if (m_redo.empty())
        return {};
    m_lastPush.invalidate();
    std::unique_ptr<Command> cmd = std::move(m_redo.back());
    m_redo.pop_back();
    const qint64 was = cmd->bytes();
    cmd->redo();
    m_bytes += cmd->bytes() - was;
    const QString name = cmd->name;
    m_undo.push_back(std::move(cmd));
    ++m_serial;
    emit changed();
    return name;
}

bool History::dropOldest()
{
    if (m_undo.empty())
        return false;
    m_bytes -= m_undo.front()->bytes();
    m_undo.pop_front();
    ++m_serial;
    emit changed();
    return true;
}

void History::clear()
{
    m_undo.clear();
    m_redo.clear();
    m_bytes = 0;
    m_lastPush.invalidate();
    ++m_serial;
    emit changed();
}
