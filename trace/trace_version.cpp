#include "trace_version.h"

#include <otf2/OTF2_GeneralDefinitions.h>

namespace trace {

QString otf2Version()
{
    return QStringLiteral("%1.%2.%3")
        .arg(OTF2_VERSION_MAJOR)
        .arg(OTF2_VERSION_MINOR)
        .arg(OTF2_VERSION_BUGFIX);
}

} // namespace trace
