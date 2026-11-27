#include "firewall.h"

#include <QDir>

#ifdef Q_OS_WIN
#include <windows.h>

#include <netfw.h>
#endif

namespace ashdesk {

#ifdef Q_OS_WIN
namespace {

bool allows(INetFwRule *rule, const QString &program) {
    BSTR group = nullptr;
    BSTR application = nullptr;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_OUT;
    NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
    const bool matches =
        SUCCEEDED(rule->get_Grouping(&group)) && group && QString::fromWCharArray(group) == QLatin1String("AshDesk") &&
        SUCCEEDED(rule->get_Enabled(&enabled)) && enabled == VARIANT_TRUE &&
        SUCCEEDED(rule->get_Direction(&direction)) && direction == NET_FW_RULE_DIR_IN &&
        SUCCEEDED(rule->get_Action(&action)) && action == NET_FW_ACTION_ALLOW &&
        SUCCEEDED(rule->get_ApplicationName(&application)) && application &&
        QDir::cleanPath(QString::fromWCharArray(application)).compare(QDir::cleanPath(program), Qt::CaseInsensitive) == 0;
    SysFreeString(group);
    SysFreeString(application);
    return matches;
}

} // namespace
#endif

FirewallRule hostFirewallRule(const QString &program) {
#ifdef Q_OS_WIN
    // Qt 的界面线程已经初始化过 COM，这里再配一对；线程按别的模型初始化过（RPC_E_CHANGED_MODE）也能用，只是不归我们收尾
    const HRESULT initialised = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    FirewallRule result = FirewallRule::Unknown;
    INetFwPolicy2 *policy = nullptr;
    INetFwRules *rules = nullptr;
    IUnknown *collection = nullptr;
    IEnumVARIANT *each = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2),
                                   reinterpret_cast<void **>(&policy))) &&
        SUCCEEDED(policy->get_Rules(&rules)) && SUCCEEDED(rules->get__NewEnum(&collection)) &&
        SUCCEEDED(collection->QueryInterface(__uuidof(IEnumVARIANT), reinterpret_cast<void **>(&each)))) {
        result = FirewallRule::Missing;
        VARIANT item;
        VariantInit(&item);
        while (result == FirewallRule::Missing && each->Next(1, &item, nullptr) == S_OK) {
            INetFwRule *rule = nullptr;
            if (item.vt == VT_DISPATCH &&
                SUCCEEDED(item.pdispVal->QueryInterface(__uuidof(INetFwRule), reinterpret_cast<void **>(&rule)))) {
                if (allows(rule, program)) result = FirewallRule::Allowed;
                rule->Release();
            }
            VariantClear(&item);
        }
    }
    for (IUnknown *object : {static_cast<IUnknown *>(each), collection, static_cast<IUnknown *>(rules),
                             static_cast<IUnknown *>(policy)})
        if (object) object->Release();
    if (SUCCEEDED(initialised)) CoUninitialize();
    return result;
#else
    Q_UNUSED(program);
    return FirewallRule::Unknown;
#endif
}

} // namespace ashdesk
