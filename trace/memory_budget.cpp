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

quint64 memoryBudgetBytes()
{
    bool ok = false;
    const quint64 envMb = qEnvironmentVariable("EDUMPI_TRACE_BUDGET_MB").toULongLong(&ok);
    if (ok && envMb > 0)
        return envMb * kMiB;
    const QSettings settings(QStringLiteral("EduMPI"), QStringLiteral("EduMPI_GUI"));
    const quint64 mb = settings.value(QStringLiteral("trace/memoryBudgetMB"), 0).toULongLong(&ok);
    if (ok && mb > 0)
        return mb * kMiB;
    return std::max<quint64>(availableMemoryBytes() / 2, 1);
}

TimelineLoadPlan planTimelineLoad(const TraceMeta &meta, int ranks, quint64 budget)
{
    TimelineLoadPlan plan;
    plan.pyramidLevels = kDefaultPyramidLevels;
    while (plan.pyramidLevels > 1 && pyramidBytes(ranks, plan.pyramidLevels) > budget / 2)
        --plan.pyramidLevels;
    const quint64 estimate = estimateTimelineBytes(meta.eventCount, meta.hasMetrics, ranks, plan.pyramidLevels);
    plan.full = double(estimate) * kBuildHeadroom <= double(budget);
    return plan;
}

qint64 maxDetailWindowNs(const TraceMeta &meta, int ranks, quint64 budget)
{
    const qint64 length = std::max<qint64>(meta.traceLengthNs, kMinWindowNs);
    const double all = double(estimateTimelineBytes(meta.eventCount, meta.hasMetrics, ranks, 0)) * kBuildHeadroom;
    const double allowed = double(budget / 4);
    if (all <= allowed)
        return length;
    const double window = double(length) * allowed / all;
    return std::clamp<qint64>(qint64(window), kMinWindowNs, length);
}

} // namespace trace
