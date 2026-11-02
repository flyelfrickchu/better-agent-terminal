#include "x11windowwatcher.h"
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QSocketNotifier>
#include <QTimer>
#include <deque>
#include <memory>
#ifdef BAT_HAVE_XCB
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#endif

namespace {
class WindowDiscovery : public QObject {
    Q_OBJECT
public:
    WindowDiscovery() {
        m_pump.setParent(this);
        m_pump.setInterval(5);
        connect(&m_pump, &QTimer::timeout, this, &WindowDiscovery::pump);
    }
    ~WindowDiscovery() override {
#ifdef BAT_HAVE_XCB
        delete m_notifier;
        if (m_connection) xcb_disconnect(m_connection);
#endif
    }
    void initialize() {
#ifdef BAT_HAVE_XCB
        int screenNumber = 0;
        m_connection = xcb_connect(nullptr, &screenNumber);
        if (xcb_connection_has_error(m_connection)) { m_error = tr("Cannot connect to the X11 display."); return; }
        auto screen = xcb_setup_roots_iterator(xcb_get_setup(m_connection));
        for (int i = 0; i < screenNumber && screen.rem; ++i) xcb_screen_next(&screen);
        if (!screen.rem) { m_error = tr("X11 screen is unavailable."); return; }
        m_root = screen.data->root;
        // Subscribe before taking a snapshot, so startup/map races cannot hide
        // a new terminal behind an initial, bounded discovery pass.
        const uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_change_window_attributes(m_connection, m_root, XCB_CW_EVENT_MASK, &mask);
        const auto atom = [this](const char *name) {
            auto *reply = xcb_intern_atom_reply(m_connection,
                xcb_intern_atom(m_connection, false, uint16_t(strlen(name)), name), nullptr);
            const auto result = reply ? reply->atom : xcb_atom_t(XCB_ATOM_NONE);
            free(reply); return result;
        };
        m_pid = atom("_NET_WM_PID"); m_type = atom("_NET_WM_WINDOW_TYPE");
        m_normal = atom("_NET_WM_WINDOW_TYPE_NORMAL"); m_transient = atom("WM_TRANSIENT_FOR");
        m_clients = atom("_NET_CLIENT_LIST");
        m_notifier = new QSocketNotifier(xcb_get_file_descriptor(m_connection), QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, &WindowDiscovery::pump);
        xcb_flush(m_connection);
#else
        m_error = tr("This build does not include X11 third-party terminal embedding.");
#endif
    }
    void watch(quint64 token, qint64 group) {
        if (!m_error.isEmpty()) { emit failed(token, m_error); return; }
#ifdef BAT_HAVE_XCB
        m_sessions.insert(token, {group, 0, false});
        m_refreshClients = true;
        wake();
#else
        Q_UNUSED(group);
#endif
    }
    void unwatch(quint64 token) {
#ifdef BAT_HAVE_XCB
        m_sessions.remove(token);
        if (m_sessions.isEmpty()) {
            m_queue.clear(); m_queued.clear(); m_candidates.clear(); m_dirty.clear();
            const uint32_t mask = 0;
            for (auto window : std::as_const(m_observed))
                xcb_change_window_attributes(m_connection, window, XCB_CW_EVENT_MASK, &mask);
            m_observed.clear(); m_refreshClients = false;
            xcb_flush(m_connection);
        }
#else
        Q_UNUSED(token);
#endif
    }
signals:
    void windowFound(quint64 token, quint32 window);
    void windowClosed(quint64 token);
    void failed(quint64 token, const QString &message);
private:
    QTimer m_pump;
    QString m_error;
#ifdef BAT_HAVE_XCB
    // Limit breadth as well as depth. PID-less, unmapped windows must never
    // turn a desktop snapshot into tens of thousands of serial round trips.
    static constexpr int MaxCandidates = 2048;
    static constexpr int MaxPending = 64;
    static constexpr int MaxDepth = 4;
    struct Session { qint64 group; xcb_window_t window; bool failed; };
    struct Search { int remaining = MaxCandidates; QDeadlineTimer deadline{2000}; };
    struct Work { xcb_window_t window; int depth; std::shared_ptr<Search> search; };
    struct Candidate {
        quint64 generation;
        int depth;
        std::shared_ptr<Search> search;
        qint64 group = -1;
        uint32_t type = 0, transient = 0;
        xcb_window_t parent = 0;
        int parentDepth = 0;
        bool hasType = false, hasTransient = false, hasParent = false;
    };
    enum Kind { Attributes, Pid, Type, Transient, Tree, Clients, Parent, ParentPid };
    struct Pending {
        unsigned sequence; Kind kind; xcb_window_t window; quint64 generation; int depth;
        std::shared_ptr<Search> search = {};
    };
    QHash<quint64, Session> m_sessions;
    QHash<xcb_window_t, Candidate> m_candidates;
    QSet<xcb_window_t> m_observed, m_queued, m_dirty;
    std::deque<Work> m_queue;
    std::deque<Pending> m_pending;
    quint64 m_generation = 0;
    xcb_connection_t *m_connection = nullptr;
    xcb_window_t m_root = 0;
    xcb_atom_t m_pid = 0, m_type = 0, m_normal = 0, m_transient = 0, m_clients = 0;
    QSocketNotifier *m_notifier = nullptr;
    bool m_refreshClients = false, m_clientsPending = false;

    bool attached(xcb_window_t window) const {
        for (const auto &session : m_sessions)
            if (session.window == window) return true;
        return false;
    }
    void wake() { if (!m_pump.isActive()) m_pump.start(); }
    void enqueue(xcb_window_t window, int depth, bool priority = false, std::shared_ptr<Search> search = {}) {
        if (m_sessions.isEmpty() || !window || window == m_root || attached(window)) return;
        if (m_candidates.contains(window)) {
            // A property/map event may arrive before an earlier reply. Never
            // let that old reply discard the only notification of readiness.
            if (priority) m_dirty.insert(window);
            return;
        }
        if (m_queued.contains(window)) return;
        if (!search) search = std::make_shared<Search>();
        if (search->remaining <= 0 || search->deadline.hasExpired()) return;
        if (m_queue.size() >= MaxCandidates) {
            if (!priority) return;
            m_queued.remove(m_queue.back().window); m_queue.pop_back();
        }
        --search->remaining;
        m_queued.insert(window);
        if (priority) m_queue.push_front({window, depth, search});
        else m_queue.push_back({window, depth, search});
        wake();
    }
    void observe(xcb_window_t window, bool required = false) {
        if (m_observed.contains(window)) return;
        if (m_observed.size() >= MaxCandidates) {
            if (!required) return;
            // Discovery observations cannot consume a terminal's exit watch.
            for (auto other : std::as_const(m_observed)) {
                if (!attached(other)) { unobserve(other); break; }
            }
        }
        const uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_change_window_attributes(m_connection, window, XCB_CW_EVENT_MASK, &mask);
        m_observed.insert(window);
    }
    void unobserve(xcb_window_t window) {
        if (m_observed.remove(window)) {
            const uint32_t mask = 0;
            xcb_change_window_attributes(m_connection, window, XCB_CW_EVENT_MASK, &mask);
        }
    }
    void finish(xcb_window_t window, bool keepObservation = false) {
        m_candidates.remove(window);
        const bool again = m_dirty.remove(window);
        if (!keepObservation && !again) unobserve(window);
        if (again) enqueue(window, 0, true); // One coalesced retry, with a fresh generation/budget.
    }
    void property(Kind kind, xcb_window_t window, xcb_atom_t atom, xcb_atom_t type, quint64 generation, xcb_window_t target = 0) {
        const auto cookie = xcb_get_property(m_connection, false, target ? target : window, atom, type, 0, 1);
        m_pending.push_back({cookie.sequence, kind, window, generation, 0});
    }
    void parent(xcb_window_t window, xcb_window_t ancestor, quint64 generation) {
        const auto cookie = xcb_query_tree(m_connection, ancestor);
        m_pending.push_back({cookie.sequence, Parent, window, generation, 0});
    }
    void tree(xcb_window_t window, int depth, std::shared_ptr<Search> search = {}) {
        if (!search) search = std::make_shared<Search>();
        if (search->remaining <= 0 || search->deadline.hasExpired()) return;
        const auto cookie = xcb_query_tree(m_connection, window);
        m_pending.push_back({cookie.sequence, Tree, window, 0, depth, search});
    }
    static uint32_t value(void *data) {
        const auto *reply = static_cast<xcb_get_property_reply_t *>(data);
        uint32_t result = 0;
        if (reply->format == 32 && xcb_get_property_value_length(reply) == 4)
            memcpy(&result, xcb_get_property_value(reply), sizeof(result));
        return result;
    }
    void accept(xcb_window_t window, Candidate candidate) {
        if (!candidate.hasType || !candidate.hasTransient || !candidate.hasParent) return;
        if (m_dirty.contains(window)) { finish(window, true); return; }
        if ((candidate.type && candidate.type != m_normal) || candidate.transient) {
            // Some applications turn an already mapped splash/transient XID
            // into their normal window. Keep its bounded readiness watch.
            finish(window, true); return;
        }
        observe(window, true);
        // An embedded terminal may create its own native child widgets. They
        // belong to its content, not additional top-level terminal windows.
        const uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes(m_connection, window, XCB_CW_EVENT_MASK, &mask);
        for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
            auto &session = it.value();
            if (session.failed || session.group != candidate.group) continue;
            if (!session.window) {
                session.window = window;
                emit windowFound(it.key(), window);
            } else if (session.window != window) {
                session.failed = true;
                emit failed(it.key(), tr("The terminal opened multiple windows. Configure one window per process for BAT embedding, then reopen this tab."));
            }
        }
        finish(window, true);
    }
    void reply(const Pending &pending, void *data) {
        if (m_sessions.isEmpty()) return;
        if (pending.kind == Clients) {
            const auto *result = static_cast<xcb_get_property_reply_t *>(data);
            if (result->type == XCB_ATOM_WINDOW && result->format == 32) {
                const auto *windows = static_cast<const xcb_window_t *>(xcb_get_property_value(result));
                const int count = xcb_get_property_value_length(result) / int(sizeof(xcb_window_t));
                auto search = std::make_shared<Search>();
                for (int i = count - 1; i >= 0 && i >= count - MaxCandidates; --i)
                    enqueue(windows[i], 0, false, search);
            } else {
                tree(m_root, 0); // No EWMH window manager (e.g. Xvfb).
            }
            return;
        }
        if (pending.kind == Tree) {
            if (pending.window != m_root && (attached(pending.window) ||
                m_candidates.contains(pending.window) || m_queued.contains(pending.window))) return;
            const auto *result = static_cast<xcb_query_tree_reply_t *>(data);
            const auto *windows = xcb_query_tree_children(result);
            const int count = xcb_query_tree_children_length(result);
            // Newest/topmost windows first; old, unmapped desktop resources
            // cannot crowd out a just-started terminal.
            // Reserve half the fallback budget for decoration-frame children.
            const int limit = qMin(count, qMax(1, pending.search->remaining / 2));
            for (int i = count - 1; i >= count - limit; --i)
                enqueue(windows[i], pending.depth, false, pending.search);
            return;
        }
        auto it = m_candidates.find(pending.window);
        if (it == m_candidates.end() || it->generation != pending.generation) return;
        auto &candidate = it.value();
        if (candidate.search->deadline.hasExpired()) { finish(pending.window); return; }
        switch (pending.kind) {
        case Attributes: {
            const auto *attributes = static_cast<xcb_get_window_attributes_reply_t *>(data);
            if (attributes->map_state != XCB_MAP_STATE_VIEWABLE || attributes->override_redirect ||
                attributes->_class != XCB_WINDOW_CLASS_INPUT_OUTPUT) { finish(pending.window); break; }
            observe(pending.window); // Catch PID/type properties set after mapping.
            property(Pid, pending.window, m_pid, XCB_ATOM_CARDINAL, candidate.generation);
            break;
        }
        case Pid: {
            const uint32_t owner = value(data);
            if (!owner) {
                // WM decoration frames have no PID. Only inspect a bounded
                // number of their viewable descendants, never other apps' trees.
                if (!m_dirty.contains(pending.window) && candidate.depth < MaxDepth)
                    tree(pending.window, candidate.depth + 1, candidate.search);
                finish(pending.window, true);
                break;
            }
            candidate.group = ::getpgid(pid_t(owner));
            bool ours = false;
            for (const auto &session : m_sessions)
                if (!session.failed && session.group == candidate.group) { ours = true; break; }
            if (!ours) { finish(pending.window); break; }
            // Only PID-less decoration frames need child notifications. A
            // process-owned splash/transient has its own content children.
            const uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
            xcb_change_window_attributes(m_connection, pending.window, XCB_CW_EVENT_MASK, &mask);
            property(Type, pending.window, m_type, XCB_ATOM_ATOM, candidate.generation);
            property(Transient, pending.window, m_transient, XCB_ATOM_WINDOW, candidate.generation);
            parent(pending.window, pending.window, candidate.generation);
            break;
        }
        case Type:
            candidate.type = value(data); candidate.hasType = true; accept(pending.window, candidate); break;
        case Transient:
            candidate.transient = value(data); candidate.hasTransient = true; accept(pending.window, candidate); break;
        case Parent: {
            const auto ancestor = static_cast<xcb_query_tree_reply_t *>(data)->parent;
            if (ancestor == m_root) {
                candidate.hasParent = true; accept(pending.window, candidate);
            } else if (!ancestor || ++candidate.parentDepth > MaxDepth) {
                finish(pending.window);
            } else {
                candidate.parent = ancestor;
                property(ParentPid, pending.window, m_pid, XCB_ATOM_CARDINAL, candidate.generation, ancestor);
            }
            break;
        }
        case ParentPid:
            // Also reject buffered child-map events from before we learned
            // their parent's PID. This mirrors discovery through PID-less WM
            // frames, without treating native content as another terminal.
            if (value(data)) finish(pending.window);
            else parent(pending.window, candidate.parent, candidate.generation);
            break;
        default: break;
        }
    }
    void event(xcb_generic_event_t *data) {
        if (m_sessions.isEmpty()) return;
        switch (data->response_type & 0x7f) {
        case XCB_MAP_NOTIFY:
            enqueue(reinterpret_cast<xcb_map_notify_event_t *>(data)->window, 0, true); break;
        case XCB_REPARENT_NOTIFY:
            enqueue(reinterpret_cast<xcb_reparent_notify_event_t *>(data)->window, 0, true); break;
        case XCB_PROPERTY_NOTIFY: {
            const auto *change = reinterpret_cast<xcb_property_notify_event_t *>(data);
            if (change->window == m_root && change->atom == m_clients) { m_refreshClients = true; wake(); }
            else if (change->atom == m_pid || change->atom == m_type || change->atom == m_transient)
                enqueue(change->window, 0, true);
            break;
        }
        case XCB_DESTROY_NOTIFY: {
            const auto window = reinterpret_cast<xcb_destroy_notify_event_t *>(data)->window;
            m_observed.remove(window); m_candidates.remove(window); m_dirty.remove(window);
            for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
                if (it->window != window || it->failed) continue;
                it->failed = true; emit windowClosed(it.key());
            }
            break;
        }
        default: break;
        }
    }
#endif
    void pump() {
#ifdef BAT_HAVE_XCB
        if (QThread::currentThread()->isInterruptionRequested()) { m_pump.stop(); return; }
        if (xcb_connection_has_error(m_connection)) {
            m_error = tr("The X11 display disconnected.");
            m_notifier->setEnabled(false); m_pump.stop();
            for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) emit failed(it.key(), m_error);
            m_sessions.clear(); return;
        }
        QElapsedTimer budget; budget.start();
        bool moreEvents = false;
        // XCB may queue events while reading replies. The timer also drains
        // that internal queue, independently of socket-read notifications.
        while (auto *data = xcb_poll_for_event(m_connection)) {
            event(data); free(data);
            if (budget.elapsed() >= 4) { moreEvents = true; break; }
        }
        if (m_refreshClients && !m_clientsPending && !m_sessions.isEmpty()) {
            const auto cookie = xcb_get_property(m_connection, false, m_root, m_clients, XCB_ATOM_WINDOW, 0, MaxCandidates);
            m_pending.push_back({cookie.sequence, Clients, m_root, 0, 0});
            m_clientsPending = true; m_refreshClients = false;
        }
        while (!m_pending.empty() && budget.elapsed() < 4) {
            const auto pending = m_pending.front();
            void *data = nullptr; xcb_generic_error_t *error = nullptr;
            if (!xcb_poll_for_reply(m_connection, pending.sequence, &data, &error)) break;
            m_pending.pop_front();
            if (pending.kind == Clients) m_clientsPending = false;
            if (data) reply(pending, data);
            else if (pending.generation && m_candidates.value(pending.window).generation == pending.generation)
                finish(pending.window);
            free(data); free(error);
        }
        // Each owned candidate can fan out into type, transient and parent
        // requests. Reserve pipeline space for those replies.
        while (!m_queue.empty() && m_pending.size() < MaxPending / 3 && budget.elapsed() < 4) {
            const auto work = m_queue.front(); m_queue.pop_front(); m_queued.remove(work.window);
            if (attached(work.window) || m_candidates.contains(work.window) || work.search->deadline.hasExpired()) continue;
            const auto generation = ++m_generation;
            m_candidates.insert(work.window, {generation, work.depth, work.search});
            const auto cookie = xcb_get_window_attributes(m_connection, work.window);
            m_pending.push_back({cookie.sequence, Attributes, work.window, generation, 0});
        }
        xcb_flush(m_connection);
        if (m_pending.empty() && m_queue.empty() && !moreEvents && !m_refreshClients) m_pump.stop();
        else wake();
#endif
    }
};
}

X11WindowWatcher *X11WindowWatcher::instance() {
    static auto *watcher = new X11WindowWatcher(QCoreApplication::instance());
    return watcher;
}

X11WindowWatcher::X11WindowWatcher(QObject *parent) : QObject(parent), m_worker(new WindowDiscovery) {
    setObjectName("externalTerminalWindowWatcher");
    auto *worker = static_cast<WindowDiscovery *>(m_worker);
    worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::started, worker, &WindowDiscovery::initialize);
    connect(&m_thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &WindowDiscovery::windowFound, this, &X11WindowWatcher::windowFound);
    connect(worker, &WindowDiscovery::windowClosed, this, &X11WindowWatcher::windowClosed);
    connect(worker, &WindowDiscovery::failed, this, &X11WindowWatcher::failed);
    m_thread.start();
}

X11WindowWatcher::~X11WindowWatcher() {
    m_thread.requestInterruption(); m_thread.quit(); m_thread.wait();
}

quint64 X11WindowWatcher::watchProcess(qint64 processGroup) {
    const auto token = ++m_nextToken;
    auto *worker = static_cast<WindowDiscovery *>(m_worker);
    QMetaObject::invokeMethod(worker, [worker, token, processGroup] { worker->watch(token, processGroup); });
    return token;
}

void X11WindowWatcher::unwatchProcess(quint64 token) {
    auto *worker = static_cast<WindowDiscovery *>(m_worker);
    QMetaObject::invokeMethod(worker, [worker, token] { worker->unwatch(token); });
}

#include "x11windowwatcher.moc"
