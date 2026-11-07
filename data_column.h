#ifndef DATA_COLUMN_H
#define DATA_COLUMN_H

#include <QString>

struct DataColumn
{
    QString proc_name;
    QString comm_type;
    int proc_rank;
    long recv_datasize;
    long send_datasize;
    float late_sender;
    float late_receiver;
    float time_diff;
};

#endif // DATA_COLUMN_H
