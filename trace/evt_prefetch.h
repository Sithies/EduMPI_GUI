#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace trace {

// Read-ahead for the event files of a whole-trace read. OTF2 reads every location's event file
// in chunks on demand, from the one reading thread: each chunk not yet in the page cache stalls
// the read (about a third of the load time of large traces). A background thread reads the
// chunks that come next a little ahead of the read progress (kChunksAhead per file), so they are
// usually in the page cache when OTF2 needs them. The data is dropped right away (one chunk
// buffer); too little or too much read-ahead only costs speed. Unix only (pread); elsewhere
// nothing is done. The thread ends with stop() or the destructor.
class EvtPrefetcher
{
public:
    // files: the event files (one per location). The read's share done so far (0 .. 1) comes
    // from the reader through setProgress.
    explicit EvtPrefetcher(QStringList files);
    ~EvtPrefetcher();
    EvtPrefetcher(const EvtPrefetcher &) = delete;
    EvtPrefetcher &operator=(const EvtPrefetcher &) = delete;

    void setProgress(double fraction) { m_progress.store(fraction, std::memory_order_relaxed); }
    void stop();

    // Event file of a location: <anchor dir>/<anchor name without .otf2>/<location>.evt.
    static QString evtFile(const QString &anchorFile, quint64 location);
    // Read-ahead beyond the estimated read position of each file.
    static constexpr qint64 kChunk = 1024 * 1024;  // OTF2's default event chunk size
    static constexpr int kChunksAhead = 2;

private:
    void run();
    bool advise(int file, qint64 upTo); // reads [m_advised[file], upTo); false if the file is gone

    QStringList m_files;
    QVector<qint64> m_sizes;
    QVector<qint64> m_advised;
    std::atomic<double> m_progress{0.0};
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop = false;
    std::thread m_thread;
};

} // namespace trace
