#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

// Carries turn-by-turn data from the projected map UI (MapPane, via the context
// property "navBridge") to the AAP Session, which forwards it to the car's
// instrument cluster (NavigationStatusService). Pure data + a signal; the
// Session does the protocol.
class NavBridge : public QObject
{
    Q_OBJECT
public:
    explicit NavBridge(QObject *parent = nullptr) : QObject(parent) {}

    // Called from QML on every navigation update.
    Q_INVOKABLE void update(bool active, int maneuverDir, const QString &instruction,
                            const QString &road, const QString &destLabel,
                            int stepMeters, int remainingMeters, int remainingSeconds,
                            const QString &eta)
    {
        m_active = active;
        m_dir = maneuverDir;
        m_instruction = instruction;
        m_road = road;
        m_destLabel = destLabel;
        m_stepMeters = stepMeters;
        m_remainingMeters = remainingMeters;
        m_remainingSeconds = remainingSeconds;
        m_eta = eta;
        m_rerouting = false; // a fresh guidance update means the new route is in
        emit updated();
    }
    Q_INVOKABLE void stop() { m_active = false; m_rerouting = false; m_upcoming.clear(); emit updated(); }

    // The next few maneuvers (first = the one update() describes), each a map
    // {type: NavigationType, exit: roundabout exit # (0 = n/a), road, text,
    //  lanes: [[{shape, hl}, ...], ...]}. Set just before update(); no signal.
    Q_INVOKABLE void setUpcoming(const QVariantList &steps) { m_upcoming = steps; }

    // Off route: a new route is being calculated. Cleared by the next update().
    Q_INVOKABLE void setRerouting(bool on)
    {
        if (on == m_rerouting)
            return;
        m_rerouting = on;
        emit updated();
    }

    bool active() const { return m_active; }
    int dir() const { return m_dir; }
    QString instruction() const { return m_instruction; }
    QString road() const { return m_road; }
    QString destLabel() const { return m_destLabel; }
    int stepMeters() const { return m_stepMeters; }
    int remainingMeters() const { return m_remainingMeters; }
    int remainingSeconds() const { return m_remainingSeconds; }
    QString eta() const { return m_eta; }
    QVariantList upcoming() const { return m_upcoming; }
    bool rerouting() const { return m_rerouting; }

signals:
    void updated();

private:
    bool m_active = false;
    bool m_rerouting = false;
    QVariantList m_upcoming;
    int m_dir = 0;
    QString m_instruction, m_road, m_destLabel, m_eta;
    int m_stepMeters = 0, m_remainingMeters = 0, m_remainingSeconds = 0;
};
