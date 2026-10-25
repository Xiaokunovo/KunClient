#include "kc/devices/UsbDeviceNotifier.h"

#include <QCoreApplication>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <dbt.h>
#endif

namespace kc {

namespace {
constexpr int kWmDeviceChange = 0x0219;
constexpr int kDbtDeviceArrival = 0x8000;
constexpr int kDbtDeviceRemoveComplete = 0x8004;
} // namespace

UsbDeviceNotifier::UsbDeviceNotifier(QObject* parent) : QObject(parent) {}

UsbDeviceNotifier* UsbDeviceNotifier::instance()
{
    static UsbDeviceNotifier* s = nullptr;
    if (!s) {
        s = new UsbDeviceNotifier(QCoreApplication::instance());
        s->initialize();
    }
    return s;
}

bool UsbDeviceNotifier::initialize()
{
#ifdef Q_OS_WIN
    if (m_initialized) return true;
    if (!QCoreApplication::instance()) return false;
    QCoreApplication::instance()->installNativeEventFilter(this);
    m_initialized = true;
    return true;
#else
    return false;
#endif
}

bool UsbDeviceNotifier::nativeEventFilter(const QByteArray& eventType, void* message,
                                          qintptr* result)
{
    Q_UNUSED(eventType);
    Q_UNUSED(result);
#ifdef Q_OS_WIN
    auto* msg = static_cast<MSG*>(message);
    if (msg && msg->message == kWmDeviceChange) {
        const int ev = static_cast<int>(msg->wParam);
        if (ev == kDbtDeviceArrival || ev == kDbtDeviceRemoveComplete)
            emit deviceChanged();
    }
#else
    Q_UNUSED(message);
#endif
    return false;
}

} // namespace kc
