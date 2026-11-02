#pragma once
#include <QObject>
#include <QThread>

// Shared discovery runs on a separate X11 connection/thread. Tokens keep late
// replies from an old terminal from affecting a replacement panel.
class X11WindowWatcher : public QObject {
    Q_OBJECT
public:
    static X11WindowWatcher *instance();
    quint64 watchProcess(qint64 processGroup);
    void unwatchProcess(quint64 token);
    ~X11WindowWatcher() override;
signals:
    void windowFound(quint64 token, quint32 window);
    void windowClosed(quint64 token);
    void failed(quint64 token, const QString &message);
private:
    explicit X11WindowWatcher(QObject *parent);
    QThread m_thread;
    QObject *m_worker;
    quint64 m_nextToken = 0;
};
