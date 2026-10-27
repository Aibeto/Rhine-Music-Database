#include "DesktopVisibility.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QLibrary>
#include <QScreen>
#include <cstdlib>
#if __has_include(<xcb/xcb.h>)
#include <xcb/xcb.h>
#define RHINE_XCB_WINDOW_QUERY 1
#endif

#ifdef RHINE_XCB_WINDOW_QUERY
namespace {
/** 动态使用 X11 的 EWMH 属性；不读取窗口标题，不新增强制运行库依赖。 */
class DesktopXcb {
  public:
    QLibrary library{"xcb", 1};
    decltype(&xcb_connect) open = resolve<decltype(&xcb_connect)>("xcb_connect");
    decltype(&xcb_disconnect) close = resolve<decltype(&xcb_disconnect)>("xcb_disconnect");
    decltype(&xcb_connection_has_error) error =
        resolve<decltype(&xcb_connection_has_error)>("xcb_connection_has_error");
    decltype(&xcb_get_setup) setup = resolve<decltype(&xcb_get_setup)>("xcb_get_setup");
    decltype(&xcb_setup_roots_iterator) roots =
        resolve<decltype(&xcb_setup_roots_iterator)>("xcb_setup_roots_iterator");
    decltype(&xcb_intern_atom) intern = resolve<decltype(&xcb_intern_atom)>("xcb_intern_atom");
    decltype(&xcb_intern_atom_reply) internReply =
        resolve<decltype(&xcb_intern_atom_reply)>("xcb_intern_atom_reply");
    decltype(&xcb_get_property) property = resolve<decltype(&xcb_get_property)>("xcb_get_property");
    decltype(&xcb_get_property_reply) propertyReply =
        resolve<decltype(&xcb_get_property_reply)>("xcb_get_property_reply");
    decltype(&xcb_get_property_value) propertyValue =
        resolve<decltype(&xcb_get_property_value)>("xcb_get_property_value");
    decltype(&xcb_get_property_value_length) propertyLength =
        resolve<decltype(&xcb_get_property_value_length)>("xcb_get_property_value_length");
    decltype(&xcb_get_geometry) geometry = resolve<decltype(&xcb_get_geometry)>("xcb_get_geometry");
    decltype(&xcb_get_geometry_reply) geometryReply =
        resolve<decltype(&xcb_get_geometry_reply)>("xcb_get_geometry_reply");
    decltype(&xcb_translate_coordinates) translate =
        resolve<decltype(&xcb_translate_coordinates)>("xcb_translate_coordinates");
    decltype(&xcb_translate_coordinates_reply) translateReply =
        resolve<decltype(&xcb_translate_coordinates_reply)>("xcb_translate_coordinates_reply");
    xcb_connection_t *connection = nullptr;
    xcb_window_t root = 0;
    DesktopXcb() {
        if (!open || !close || !error || !setup || !roots || !intern || !internReply || !property ||
            !propertyReply || !propertyValue || !propertyLength || !geometry || !geometryReply ||
            !translate || !translateReply) {
            return;
        }
        connection = open(nullptr, nullptr);
        if (error(connection)) {
            close(connection);
            connection = nullptr;
            return;
        }
        const auto screens = roots(setup(connection));
        if (screens.data) {
            root = screens.data->root;
        }
    }
    ~DesktopXcb() {
        if (connection) {
            close(connection);
        }
    }
    template <typename Function> Function resolve(const char *name) {
        return reinterpret_cast<Function>(library.resolve(name));
    }
    xcb_atom_t atom(const QByteArray &name) {
        auto reply =
            internReply(connection, intern(connection, false, name.size(), name.constData()), nullptr);
        const auto value = reply ? reply->atom : 0;
        std::free(reply);
        return value;
    }
    QList<quint32> values(xcb_window_t window, const QByteArray &name) {
        QList<quint32> result;
        auto reply = propertyReply(
            connection, property(connection, false, window, atom(name), XCB_GET_PROPERTY_TYPE_ANY, 0, 128),
            nullptr);
        if (reply && reply->format == 32) {
            const auto data = static_cast<const quint32 *>(propertyValue(reply));
            for (int index = 0; index < propertyLength(reply) / 4; ++index) {
                result.append(data[index]);
            }
        }
        std::free(reply);
        return result;
    }
};
}
#endif

/** Wayland 未提供通用的外部窗口查询；X11 读取前台窗口状态与所在屏幕，不改变焦点。 */
bool platformDesktopCovered(const QString &screenName, qint64 interfacePid, const QRect &screenGeometry) {
#ifdef RHINE_XCB_WINDOW_QUERY
    if (QGuiApplication::platformName() != "xcb") {
        return false;
    }
    static DesktopXcb query;
    if (!query.connection || query.error(query.connection)) {
        return false;
    }
    const auto active = query.values(query.root, "_NET_ACTIVE_WINDOW").value(0);
    const auto pid = query.values(active, "_NET_WM_PID").value(0);
    if (!active || (interfacePid > 0 && pid == quint32(interfacePid))) {
        return false;
    }
    const auto windows = query.values(query.root, "_NET_CLIENT_LIST_STACKING");
    const auto desktop = query.values(query.root, "_NET_CURRENT_DESKTOP").value(0);
    for (auto iterator = windows.crbegin(); iterator != windows.crend(); ++iterator) {
        const auto candidate = *iterator;
        const auto ownerPid = query.values(candidate, "_NET_WM_PID").value(0);
        if (ownerPid == quint32(interfacePid) && interfacePid > 0) {
            return false;
        }
        const auto windowDesktop = query.values(candidate, "_NET_WM_DESKTOP").value(0, desktop);
        if (windowDesktop != desktop && windowDesktop != 0xffffffffu) {
            continue;
        }
        const auto states = query.values(candidate, "_NET_WM_STATE");
        if (states.contains(query.atom("_NET_WM_STATE_HIDDEN"))) {
            continue;
        }
        const bool maximized = states.contains(query.atom("_NET_WM_STATE_MAXIMIZED_VERT")) &&
                               states.contains(query.atom("_NET_WM_STATE_MAXIMIZED_HORZ"));
        if (!maximized && !states.contains(query.atom("_NET_WM_STATE_FULLSCREEN"))) {
            continue;
        }
        auto geometry =
            query.geometryReply(query.connection, query.geometry(query.connection, candidate), nullptr);
        auto position = query.translateReply(
            query.connection, query.translate(query.connection, candidate, query.root, 0, 0), nullptr);
        bool covered = false;
        if (geometry && position) {
            const QRectF bounds(position->dst_x, position->dst_y, geometry->width, geometry->height);
            for (auto screen : QGuiApplication::screens()) {
                if (screen->name() == screenName &&
                    (!screenGeometry.isValid() || screen->geometry() == screenGeometry)) {
                    const auto logical = screen->geometry();
                    const auto scale = screen->devicePixelRatio();
                    const QRectF physical(logical.x() * scale, logical.y() * scale, logical.width() * scale,
                                          logical.height() * scale);
                    covered = physical.contains(bounds.center());
                    break;
                }
            }
        }
        std::free(geometry);
        std::free(position);
        if (covered) {
            return true;
        }
    }
    return false;
#else
    Q_UNUSED(screenName);
    Q_UNUSED(interfacePid);
    Q_UNUSED(screenGeometry);
    return false;
#endif
}

qint64 platformDesktopForegroundPid() {
    return 0;
}
