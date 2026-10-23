#include <QFile>
#include <QLibrary>
#include <QList>
#include <QObject>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QTimer>
#include <QVariant>

#include <algorithm>

// Minimal NVML binding, loaded only while the status popup is open. Using NVML
// directly avoids spawning nvidia-smi once a second, and keeps the GPU numbers
// independent from the Plasma system-monitor service (whose GPU sensors can be
// unavailable on hybrid laptops).
class Nvml
{
public:
    struct Utilization {
        unsigned int gpu;
        unsigned int memory;
    };

    bool load()
    {
        if (m_ready) {
            return true;
        }
        if (!m_library.load()) {
            return false;
        }

        m_init = reinterpret_cast<Init>(m_library.resolve("nvmlInit_v2"));
        if (!m_init) {
            m_init = reinterpret_cast<Init>(m_library.resolve("nvmlInit"));
        }
        m_shutdown = reinterpret_cast<Shutdown>(m_library.resolve("nvmlShutdown"));
        m_deviceCount = reinterpret_cast<DeviceGetCount>(m_library.resolve("nvmlDeviceGetCount_v2"));
        if (!m_deviceCount) {
            m_deviceCount = reinterpret_cast<DeviceGetCount>(m_library.resolve("nvmlDeviceGetCount"));
        }
        m_deviceHandle = reinterpret_cast<DeviceGetHandle>(m_library.resolve("nvmlDeviceGetHandleByIndex_v2"));
        if (!m_deviceHandle) {
            m_deviceHandle = reinterpret_cast<DeviceGetHandle>(m_library.resolve("nvmlDeviceGetHandleByIndex"));
        }
        m_utilization = reinterpret_cast<DeviceGetUtilization>(m_library.resolve("nvmlDeviceGetUtilizationRates"));
        m_temperature = reinterpret_cast<DeviceGetTemperature>(m_library.resolve("nvmlDeviceGetTemperature"));

        if (!m_init || !m_deviceCount || !m_deviceHandle || !m_utilization || !m_temperature
            || m_init() != 0) {
            m_library.unload();
            resetFunctions();
            return false;
        }
        m_ready = true;
        return true;
    }

    void unload()
    {
        if (!m_library.isLoaded()) {
            return;
        }
        if (m_ready && m_shutdown) {
            m_shutdown();
        }
        m_ready = false;
        m_library.unload();
        resetFunctions();
    }

    bool available() const { return m_ready; }

    int usage()
    {
        if (!m_ready) {
            return 0;
        }
        unsigned int count = 0;
        if (m_deviceCount(&count) != 0) {
            return 0;
        }
        int maximum = 0;
        for (unsigned int i = 0; i < count; ++i) {
            void *device = nullptr;
            if (m_deviceHandle(i, &device) != 0 || !device) {
                continue;
            }
            Utilization utilization = {};
            if (m_utilization(device, &utilization) != 0) {
                continue;
            }
            maximum = std::max(maximum, int(utilization.gpu));
        }
        return maximum;
    }

    int temperature()
    {
        if (!m_ready) {
            return 0;
        }
        unsigned int count = 0;
        if (m_deviceCount(&count) != 0) {
            return 0;
        }
        int maximum = 0;
        for (unsigned int i = 0; i < count; ++i) {
            void *device = nullptr;
            if (m_deviceHandle(i, &device) != 0 || !device) {
                continue;
            }
            unsigned int value = 0;
            // NVML_TEMPERATURE_GPU == 0.
            if (m_temperature(device, 0, &value) == 0) {
                maximum = std::max(maximum, int(value));
            }
        }
        return maximum;
    }

private:
    using Init = int (*)();
    using Shutdown = int (*)();
    using DeviceGetCount = int (*)(unsigned int *);
    using DeviceGetHandle = int (*)(unsigned int, void **);
    using DeviceGetUtilization = int (*)(void *, Utilization *);
    using DeviceGetTemperature = int (*)(void *, int, unsigned int *);

    void resetFunctions()
    {
        m_init = nullptr;
        m_shutdown = nullptr;
        m_deviceCount = nullptr;
        m_deviceHandle = nullptr;
        m_utilization = nullptr;
        m_temperature = nullptr;
    }

    QLibrary m_library{QStringLiteral("libnvidia-ml.so.1")};
    bool m_ready = false;
    Init m_init = nullptr;
    Shutdown m_shutdown = nullptr;
    DeviceGetCount m_deviceCount = nullptr;
    DeviceGetHandle m_deviceHandle = nullptr;
    DeviceGetUtilization m_utilization = nullptr;
    DeviceGetTemperature m_temperature = nullptr;
};

class SystemStats : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(int cpuUsage READ cpuUsage NOTIFY cpuUsageChanged)
    Q_PROPERTY(int gpuUsage READ gpuUsage NOTIFY gpuUsageChanged)
    Q_PROPERTY(int gpuTemp READ gpuTemp NOTIFY gpuTempChanged)
    Q_PROPERTY(bool gpuAvailable READ gpuAvailable NOTIFY gpuAvailableChanged)
    Q_PROPERTY(int ramPercent READ ramPercent NOTIFY ramPercentChanged)
    Q_PROPERTY(double ramUsedGb READ ramUsedGb NOTIFY ramChanged)
    Q_PROPERTY(double ramTotalGb READ ramTotalGb NOTIFY ramChanged)

public:
    explicit SystemStats(QObject *parent = nullptr)
        : QObject(parent)
    {
        m_timer.setInterval(1000);
        m_timer.setSingleShot(false);
        connect(&m_timer, &QTimer::timeout, this, &SystemStats::update);
    }

    bool active() const { return m_active; }
    int cpuUsage() const { return m_cpuUsage; }
    int gpuUsage() const { return m_gpuUsage; }
    int gpuTemp() const { return m_gpuTemp; }
    bool gpuAvailable() const { return m_gpuAvailable; }
    int ramPercent() const { return m_ramPercent; }
    double ramUsedGb() const { return m_ramUsedGb; }
    double ramTotalGb() const { return m_ramTotalGb; }

    void setActive(bool active)
    {
        if (m_active == active) {
            return;
        }
        m_active = active;
        if (m_active) {
            m_nvml.load();
            // Seed the CPU delta and fill RAM/GPU immediately.
            update();
            m_timer.start();
        } else {
            m_timer.stop();
            m_nvml.unload();
        }
        Q_EMIT activeChanged();
    }

Q_SIGNALS:
    void activeChanged();
    void cpuUsageChanged();
    void gpuUsageChanged();
    void gpuTempChanged();
    void gpuAvailableChanged();
    void ramPercentChanged();
    void ramChanged();

private:
    struct CpuSample {
        quint64 total = 0;
        quint64 idle = 0;
    };

    void update()
    {
        updateCpu();
        updateMemory();
        updateGpu();
    }

    void updateCpu()
    {
        QFile file(QStringLiteral("/proc/stat"));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return;
        }
        const QByteArray line = file.readLine();
        const QList<QByteArray> parts = line.simplified().split(' ');
        if (parts.size() < 5 || parts.at(0) != "cpu") {
            return;
        }

        quint64 values[8] = {};
        for (int i = 0; i < 8 && i + 1 < parts.size(); ++i) {
            values[i] = parts.at(i + 1).toULongLong();
        }
        const quint64 idle = values[3] + values[4];
        quint64 total = 0;
        for (quint64 value : values) {
            total += value;
        }

        if (m_cpuPrevious.total > 0 && total > m_cpuPrevious.total) {
            const quint64 totalDelta = total - m_cpuPrevious.total;
            const quint64 idleDelta = idle - m_cpuPrevious.idle;
            const int usage = totalDelta > 0
                ? qRound(100.0 * double(totalDelta - idleDelta) / double(totalDelta))
                : 0;
            const int clamped = std::clamp(usage, 0, 100);
            if (m_cpuUsage != clamped) {
                m_cpuUsage = clamped;
                Q_EMIT cpuUsageChanged();
            }
        }
        m_cpuPrevious = {total, idle};
    }

    void updateMemory()
    {
        QFile file(QStringLiteral("/proc/meminfo"));
        if (!file.open(QIODevice::ReadOnly)) {
            return;
        }

        const QByteArray contents = file.readAll();
        quint64 totalKb = 0;
        quint64 availableKb = 0;
        const QList<QByteArray> lines = contents.split('\n');
        for (const QByteArray &line : lines) {
            const int colon = line.indexOf(':');
            if (colon < 0) {
                continue;
            }
            const QByteArray key = line.left(colon).trimmed();
            const quint64 kb = line.mid(colon + 1).simplified().split(' ').value(0).toULongLong();
            if (key == "MemTotal") {
                totalKb = kb;
            } else if (key == "MemAvailable") {
                availableKb = kb;
            }
        }
        if (totalKb == 0) {
            return;
        }

        const quint64 usedKb = totalKb > availableKb ? totalKb - availableKb : 0;
        const int percent = std::clamp(qRound(100.0 * double(usedKb) / double(totalKb)), 0, 100);
        const double usedGb = double(usedKb) / (1024.0 * 1024.0);
        const double totalGb = double(totalKb) / (1024.0 * 1024.0);

        if (m_ramPercent != percent) {
            m_ramPercent = percent;
            Q_EMIT ramPercentChanged();
        }
        if (!qFuzzyCompare(m_ramUsedGb, usedGb) || !qFuzzyCompare(m_ramTotalGb, totalGb)) {
            m_ramUsedGb = usedGb;
            m_ramTotalGb = totalGb;
            Q_EMIT ramChanged();
        }
    }

    void updateGpu()
    {
        const bool available = m_nvml.available();
        if (m_gpuAvailable != available) {
            m_gpuAvailable = available;
            Q_EMIT gpuAvailableChanged();
        }
        if (!available) {
            return;
        }

        const int usage = std::clamp(m_nvml.usage(), 0, 100);
        const int temp = std::max(0, m_nvml.temperature());
        if (m_gpuUsage != usage) {
            m_gpuUsage = usage;
            Q_EMIT gpuUsageChanged();
        }
        if (m_gpuTemp != temp) {
            m_gpuTemp = temp;
            Q_EMIT gpuTempChanged();
        }
    }

    QTimer m_timer;
    Nvml m_nvml;
    bool m_active = false;
    int m_cpuUsage = 0;
    int m_gpuUsage = 0;
    int m_gpuTemp = 0;
    bool m_gpuAvailable = false;
    int m_ramPercent = 0;
    double m_ramUsedGb = 0.0;
    double m_ramTotalGb = 0.0;
    CpuSample m_cpuPrevious;
};

class SystemStatsPlugin : public QQmlExtensionPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)

public:
    void registerTypes(const char *uri) override
    {
        qmlRegisterType<SystemStats>(uri, 1, 0, "SystemStats");
    }
};

#include "systemstats.moc"
