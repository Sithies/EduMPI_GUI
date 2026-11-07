#include <QtTest>

#include "trace_version.h"

class TstTraceBuild : public QObject
{
    Q_OBJECT
private slots:
    void linksOtf2()
    {
        QVERIFY(trace::otf2Version().startsWith("3."));
    }
};

QTEST_GUILESS_MAIN(TstTraceBuild)
#include "tst_trace_build.moc"
