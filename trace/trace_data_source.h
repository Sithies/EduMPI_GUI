#pragma once

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>

#include <atomic>

#include "data_column.h"
#include "otf2_trace_reader.h"

// Drop-in replacement for Database_Thread that serves a local OTF2 trace instead of the
// database. Slot and signal names / argument types match Database_Thread so that
// Cluster_Architecture can connect it with the same connect list; the observable
// behaviour of updateData / showDataFromTimePeriod / detailed_p2p_Query is mirrored.
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

private:
    // Emits updateDataReady with the rows of seconds [from, to] (empty if from > to).
    void emitRows(int from, int to);
    // Emits the detail signals (functions string, lists, matrices) for seconds [from, to].
    void emitDetails(int from, int to);

    QString m_input;
    trace::TraceSummary m_summary;
    trace::TraceMeta m_meta;
    bool m_loaded = false;
    int m_matrixSize = 0;
    QDateTime m_start;

    bool m_started = false; // setTimestamp(start, true) already emitted
    int m_current = -1;     // current second of the live-style replay

    std::atomic_bool m_cancel{false};
};
