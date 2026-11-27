#pragma once

#include <QString>

namespace ashdesk {

enum class FirewallRule { Unknown, Missing, Allowed };

// 安装脚本（packaging/windows/install.ps1 的 Add-AshFirewallRules）建的规则在不在：
// 规则组 AshDesk 里有一条已启用、放行入站、指向 program 的规则才算 Allowed。
// 查不了（不是 Windows、防火墙接口出错）返回 Unknown。在调用线程上同步查，一次几十到几百毫秒
FirewallRule hostFirewallRule(const QString &program);

} // namespace ashdesk
