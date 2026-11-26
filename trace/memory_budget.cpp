#include "memory_budget.h"

#include <QFile>
#include <QSettings>

#include <algorithm>

#include "timeline_store.h"

#if defined(Q_OS_MACOS)
#include <mach/mach.h>
#endif

namespace trace {

namespace {
constexpr quint64 kMiB = 1024ull * 1024ull;
constexpr quint64 kFallbackBytes = 2048ull * kMiB;
constexpr double kBuildHeadroom = 1.5;
// Process memory per kept byte during a complete load (vector growth, pending links, the
// aggregator's per-second tables), measured with a 1 GB / 1024-rank trace: ~2.
constexpr double kLoadHeadroom = 2.0;
constexpr qint64 kMinWindowNs = 10'000; // 10 us, the smallest zoom window
} // namespace

quint64 availableMemoryBytes()
{
#if defined(Q_OS_MACOS)
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t pageSize = 0;
    const mach_port_t host = mach_host_self();
    const bool ok = host_page_size(host, &pageSize) == KERN_SUCCESS
        && host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS;
    mach_port_deallocate(mach_task_self(), host);
    if (ok) {
        const quint64 bytes = (quint64(vm.free_count) + quint64(vm.inactive_count)) * quint64(pageSize);
        if (bytes > 0)
            return bytes;
    }
#elif defined(Q_OS_LINUX)
    QFile f(QStringLiteral("/proc/meminfo"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!f.atEnd()) {
            const QByteArray line = f.readLine();
            if (!line.startsWith("MemAvailable:"))
                continue;
            const QList<QByteArray> parts = line.simplified().split(' ');
            bool ok = false;
            const quint64 kb = parts.size() >= 2 ? parts[1].toULongLong(&ok) : 0;
            if (ok && kb > 0)
                return kb * 1024ull;
            break;
        }
    }
#endif
    return kFallbackBytes;
}

MemoryBudget resolveMemoryBudget()
{
    MemoryBudget b;
    b.availableBytes = availableMemoryBytes();
    bool ok = false;
    const quint64 envMb = qEnvironmentVariable("EDUMPI_TRACE_BUDGET_MB").toULongLong(&ok);
    if (ok && envMb > 0) {
        b.bytes = envMb * kMiB;
        b.source = MemoryBudget::Environment;
        return b;
    }
    const QSettings settings(QStringLiteral("EduMPI"), QStringLiteral("EduMPI_GUI"));
    const quint64 mb = settings.value(QStringLiteral("trace/memoryBudgetMB"), 0).toULongLong(&ok);
    if (ok && mb > 0) {
        b.bytes = mb * kMiB;
        b.source = MemoryBudget::Setting;
        return b;
    }
    b.bytes = std::max<quint64>(b.availableBytes / 2, 1);
    b.source = MemoryBudget::Automatic;
    return b;
}

quint64 memoryBudgetBytes()
{
    return resolveMemoryBudget().bytes;
}

int memoryBudgetSettingMB()
{
    const QSettings settings(QStringLiteral("EduMPI"), QStringLiteral("EduMPI_GUI"));
    bool ok = false;
    const int mb = settings.value(QStringLiteral("trace/memoryBudgetMB"), 0).toInt(&ok);
    return ok && mb > 0 ? mb : 0;
}

void setMemoryBudgetSettingMB(int mb)
{
    QSettings settings(QStringLiteral("EduMPI"), QStringLiteral("EduMPI_GUI"));
    settings.setValue(QStringLiteral("trace/memoryBudgetMB"), std::max(0, mb));
}

TimelineLoadPlan planTimelineLoad(int ranks, quint64 budget)
{
    TimelineLoadPlan plan;
    plan.pyramidLevels = kDefaultPyramidLevels;
    while (plan.pyramidLevels > 1 && pyramidBytes(ranks, plan.pyramidLevels) > budget / 2)
        --plan.pyramidLevels;
    const quint64 pyramid = pyramidBytes(ranks, plan.pyramidLevels);
    const quint64 rest = budget > pyramid ? budget - pyramid : 0;
    plan.downgradeCapBytes = std::max<quint64>(quint64(double(rest) / kLoadHeadroom), 1);
    return plan;
}

qint64 maxDetailWindowNs(const TraceMeta &meta, int ranks, quint64 budget, double bytesPerEvent)
{
    const qint64 length = std::max<qint64>(meta.traceLengthNs, kMinWindowNs);
    const double bytes = bytesPerEvent > 0 ? bytesPerEvent * double(meta.eventCount)
                                           : double(estimateTimelineBytes(meta.eventCount, meta.hasMetrics, ranks, 0));
    const double all = bytes * kBuildHeadroom;
    const double allowed = double(budget / 4);
    if (all <= allowed)
        return length;
    const double window = double(length) * allowed / all;
    return std::clamp<qint64>(qint64(window), kMinWindowNs, length);
}

} // namespace trace
