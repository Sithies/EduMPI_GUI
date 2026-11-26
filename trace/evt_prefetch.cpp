#include "evt_prefetch.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

namespace trace {

EvtPrefetcher::EvtPrefetcher(QStringList files) : m_files(std::move(files))
{
    m_sizes.reserve(m_files.size());
    for (const QString &f : std::as_const(m_files))
        m_sizes.append(QFileInfo(f).size());
    m_advised = QVector<qint64>(m_files.size(), 0);
#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
    if (!m_files.isEmpty())
        m_thread = std::thread([this] { run(); });
#endif
}

EvtPrefetcher::~EvtPrefetcher()
{
    stop();
}

void EvtPrefetcher::stop()
{
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

QString EvtPrefetcher::evtFile(const QString &anchorFile, quint64 location)
{
    const QFileInfo anchor(anchorFile);
    return anchor.absoluteDir().filePath(anchor.completeBaseName() + QLatin1Char('/') + QString::number(location)
                                         + QStringLiteral(".evt"));
}

bool EvtPrefetcher::advise(int file, qint64 upTo)
{
#ifdef Q_OS_UNIX
    const qint64 from = m_advised[file];
    if (upTo <= from)
        return true;
    const int fd = ::open(QFile::encodeName(m_files[file]).constData(), O_RDONLY);
    if (fd < 0)
        return false;
    // Reading the range pulls it into the page cache (advisory hints such as macOS F_RDADVISE
    // showed no effect); the data itself is dropped.
    static thread_local std::vector<char> buffer = std::vector<char>(size_t(kChunk));
    for (qint64 off = from; off < upTo;) {
        const ssize_t n = ::pread(fd, buffer.data(), size_t(std::min<qint64>(kChunk, upTo - off)), off_t(off));
        if (n <= 0)
            break;
        off += n;
    }
    ::close(fd);
    m_advised[file] = upTo;
    return true;
#else
    Q_UNUSED(file);
    Q_UNUSED(upTo);
    return false;
#endif
}

void EvtPrefetcher::run()
{
    using namespace std::chrono_literals;
    for (;;) {
        // Every file is read about in step with the whole read (events are spread over the run):
        // hint the chunks up to kChunksAhead beyond the estimated position.
        const double p = std::clamp(m_progress.load(std::memory_order_relaxed), 0.0, 1.0);
        bool more = false;
        for (int i = 0; i < m_files.size(); ++i) {
            const qint64 size = m_sizes[i];
            const qint64 pos = qint64(std::floor(p * double(size) / kChunk)) * kChunk;
            const qint64 want = std::min(size, pos + kChunksAhead * kChunk);
            if (want > m_advised[i] && !advise(i, want))
                m_advised[i] = size; // gone: never again
            more = more || m_advised[i] < size;
        }
        std::unique_lock lock(m_mutex);
        if (!more || m_wake.wait_for(lock, 20ms, [this] { return m_stop; }))
            return;
    }
}

} // namespace trace
