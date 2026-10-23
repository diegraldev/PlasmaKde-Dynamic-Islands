#include "micstate.h"

#include <QQmlExtensionPlugin>
#include <qqml.h>

class MicStatePlugin : public QQmlExtensionPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)

public:
    void registerTypes(const char *uri) override
    {
        qmlRegisterType<MicState>(uri, 1, 0, "MicState");
    }
};

#include "micstate_plugin.moc"
