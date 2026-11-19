#pragma once

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>

#include <atomic>
#include <memory>

#include "data_column.h"
#include "otf2_trace_reader.h"
#include "timeline_snapshot.h"

// Drop-in replacement for Database_Thread that serves a local OTF2 trace instead of the
// database. Slot and signal names / argument types match Database_Thread so that
// Cluster_Architecture can connect it with the same connect list; the observable
// behaviour of updateData / showDataFromTimePeriod / detailed_p2p_Query is mirrored.
//
// Stage 2: the load also builds a trace::TimelineStore within trace::memoryBudgetBytes():
// a complete store if it fits (full mode), else a pyramid-only overview (windowed mode) whose
// details are loaded on demand by requestDetail. Results arrive via timelineReady.
class Trace_Data_Source : public QObject
{
    Q_OBJECT

public:
    explicit Trace_Data_Source(const QString &tracePathOrUrl, QObject *parent = nullptr);

    // Local time of the trace's realtime start; today 00:00:01 local if the trace has none.
    QDateTime startTimestamp() const;

public slots:
    void connectToDB();
    void threadbuildClusterComponents();
    void updateData(const int &time_display);
    void showDataFromTimePeriod(const QDateTime timestampA, const QDateTime timestampB);
    void clearDatabase();
    void selectEndTimestamp();
    void set_end_timestamp_db(QDateTime timestamp);
    void reset_actual_timestamp();
    void set_thread_running(bool running);

    // Windowed mode only (ignored in full mode / before the load): re-reads the trace for
    // [fromNs, toNs] widened by 50 % on each side (capped by the memory budget) and emits
    // timelineReady with the window store. Thread-safe: may be called from any thread; the
    // read runs on this object's thread. Only the latest request is answered; a read still
    // running for an older request is cancelled.
    void requestDetail(qint64 fromNs, qint64 toNs);
    // Emits the same data / detail signals as showDataFromTimePeriod for [fromNs, toNs]:
    // exact (TimelineStore::aggregate) if the full store or the loaded detail window covers
    // it, else whole seconds floor(from) .. ceil(to) - 1; then traceWindowShown with the
    // window actually shown.
    void showTraceWindow(qint64 fromNs, qint64 toNs);

signals:
    void connectedToDB(const bool &success);
    void clusterComponentsReady(const QMap<QString, QVector<int>> &map);
    void updateDataReady(const QList<DataColumn> &list);
    void updateDetailedP2P(QList<QVariantList>);
    void updateDetailedColl(QList<QVariantList>);
    void dbCleared();
    // bool = true for the start timestamp, false for the end timestamp
    void setTimestamp(QDateTime timestamp, bool start);

    void setFunctionsString(QString string);
    void setCommMatrixP2PSend(QVector<QVector<long>>);
    void setCommMatrixP2PRecv(QVector<QVector<long>>);
    void setCommMatrixCollSend(QVector<QVector<long>>);
    void setCommMatrixCollRecv(QVector<QVector<long>>);
    void setCommMatrixTotalSend(QVector<QVector<long>>);

    void loadFailed(QString message);
    void traceInfoReady(QString name, int ranks, double durationSeconds, int unmatched);

    // After the load (before clusterComponentsReady) and after each detail load.
    void timelineReady(trace::TimelineSnapshot snapshot);
    void traceWindowShown(qint64 fromNs, qint64 toNs, bool exact);

private:
    // Emits updateDataReady with the rows of seconds [from, to] (empty if from > to).
    void emitRows(int from, int to);
    // Emits the detail signals (functions string, lists, matrices) for seconds [from, to].
    void emitDetails(int from, int to);
    void emitBucketRows(const QVector<const trace::BucketRow *> &rows);
    void emitDetailRows(const QVector<const trace::DetailRow *> &details);
    void processDetailRequest();

    QString m_input;
    trace::TraceSummary m_summary;
    trace::TraceMeta m_meta;
    bool m_loaded = false;
    int m_matrixSize = 0;
    QDateTime m_start;

    bool m_started = false; // setTimestamp(start, true) already emitted
    int m_current = -1;     // current second of the live-style replay

    std::atomic_bool m_cancel{false};

    // Timeline (stage 2); written by the load, used on this object's thread.
    QString m_anchor;
    quint64 m_budget = 0;
    int m_rankCount = 0;
    qint64 m_maxDetailWindowNs = 0; // trace::maxDetailWindowNs for this trace and budget
    std::shared_ptr<const trace::TimelineStore> m_overview; // complete in full mode
    std::shared_ptr<const trace::TimelineStore> m_detailStore; // latest detail window
    std::atomic_bool m_windowed{false};

    // Detail requests (requestDetail may run on another thread).
    QMutex m_detailMutex;
    qint64 m_pendingFrom = 0;
    qint64 m_pendingTo = 0;
    std::atomic<quint64> m_detailGen{0}; // number of requests so far
    quint64 m_detailDone = 0;            // last request taken by processDetailRequest
    std::atomic_bool m_detailCancel{false};
};
