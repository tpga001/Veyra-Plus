#include "BrowserPreviewHost.h"
#include "veyra/ui/QmlPlayerBridge.h"
#include "veyra/Log.h"
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QTimer>
#include <QVariantMap>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <format>

namespace veyra::ui {
namespace {
struct PaintState { bool hasFrame = false; };
LRESULT CALLBACK previewProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        const auto* state = reinterpret_cast<const PaintState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!state || !state->hasFrame) {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            EndPaint(window, &paint);
        } else ValidateRect(window, nullptr);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
struct Cover { QRectF rect; qreal radius; };
void collectCovers(QQuickItem* item, QList<Cover>& covers) {
    if (!item || !item->isVisible() || item->opacity() <= 0.0) return;
    const bool tooltip = item->inherits("QQuickPopupItem") && item->parent() && item->parent()->inherits("QQuickToolTip");
    if (tooltip || item->objectName() == QLatin1String("videoCover") || item->property("videoCover").toBool()) {
        const QRectF rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        const qreal scale = item->width() > 0 ? rect.width() / item->width() : 1.0;
        covers << Cover{rect, item->property("coverRadius").toReal() * scale};
    }
    for (auto* child : item->childItems()) collectCovers(child, covers);
}
}

struct BrowserPreviewHost::Impl {
    QPointer<QQuickWindow> window;
    QmlPlayerBridge& bridge;
    HWND native = nullptr;
    PaintState paint;
    QRect geometry;
    QList<QRect> regionKey;
    bool stopped = false, syncing = false;
    Impl(QQuickWindow& window, QmlPlayerBridge& bridge) : window(&window), bridge(bridge) {}

    void syncCovers(const QRectF& sceneRect, qreal dpr) {
        QList<Cover> covers;
        auto* content = window->contentItem();
        collectCovers(content && content->parentItem() ? content->parentItem() : content, covers);
        RECT client{};
        if (!GetClientRect(native, &client)) return;
        const int width = std::max(1L, client.right), height = std::max(1L, client.bottom);
        QList<QRect> holes;
        for (const auto& cover : covers) {
            const QRectF local = cover.rect.translated(-sceneRect.topLeft());
            if (!local.intersects(QRectF(0, 0, sceneRect.width(), sceneRect.height()))) continue;
            holes << QRect(int(std::floor(local.x() * dpr)), int(std::floor(local.y() * dpr)),
                int(std::ceil(local.width() * dpr)) + 1, int(std::ceil(local.height() * dpr)) + 1);
            holes << QRect(0, 0, int(std::round(cover.radius * dpr * 2)), 0);
        }
        auto key = holes;
        key << QRect(0, 0, width, height);
        if (key == regionKey) return;
        HRGN region = CreateRectRgn(0, 0, width, height);
        for (int i = 0; i < holes.size(); i += 2) {
            const QRect& hole = holes[i];
            const int diameter = holes[i + 1].width();
            HRGN subtract = diameter > 0
                ? CreateRoundRectRgn(hole.x(), hole.y(), hole.x() + hole.width(), hole.y() + hole.height(), diameter, diameter)
                : CreateRectRgn(hole.x(), hole.y(), hole.x() + hole.width(), hole.y() + hole.height());
            CombineRgn(region, region, subtract, RGN_DIFF);
            DeleteObject(subtract);
        }
        if (SetWindowRgn(native, region, TRUE)) regionKey = std::move(key);
        else DeleteObject(region);
    }
};

BrowserPreviewHost::BrowserPreviewHost(QQuickWindow& window, QmlPlayerBridge& bridge)
    : impl_(std::make_unique<Impl>(window, bridge)) {
    static const bool registered = [] {
        WNDCLASSW cls{};
        cls.lpfnWndProc = previewProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"VeyraBrowserPreviewHost";
        return RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (registered) impl_->native = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"VeyraBrowserPreviewHost", L"",
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 16, 16,
        reinterpret_cast<HWND>(window.winId()), nullptr, GetModuleHandleW(nullptr), &impl_->paint);
    if (!impl_->native) log::error("browser-preview-host", std::format("create failed error={}", GetLastError()));
    connect(&window, &QQuickWindow::afterAnimating, this, [this] { sync(); });
    connect(&window, &QWindow::visibilityChanged, this, [this] { sync(); });
    connect(&window, &QWindow::widthChanged, this, [this] { sync(); });
    connect(&window, &QWindow::heightChanged, this, [this] { sync(); });
    connect(&window, &QWindow::screenChanged, this, [this] { sync(); });
    connect(&bridge, &QmlPlayerBridge::browserChanged, this, [this] { sync(); });
    connect(&bridge, &QmlPlayerBridge::browserPreviewChanged, this, [this] { sync(); });
    connect(&bridge, &QmlPlayerBridge::snapshotChanged, this, [this] { sync(); });
    QTimer::singleShot(0, this, [this] { sync(); });
}

void BrowserPreviewHost::sync() {
    auto& state = *impl_;
    if (state.stopped || state.syncing || !state.window || !state.native) return;
    state.syncing = true;
    const QRectF logical = state.window->property("browserPreviewRect").toRectF();
    const auto before = state.bridge.browserPreview();
    const bool owned = before.value("sessionId").toULongLong() != 0;
    const bool wanted = owned && state.bridge.sourceKind() == QLatin1String("browser") &&
        state.window->isVisible() && state.window->visibility() != QWindow::Minimized &&
        logical.width() > 1 && logical.height() > 1;
    if (wanted) {
        if (state.paint.hasFrame && !before.value("hasFrame").toBool()) {
            state.paint.hasFrame = false;
            InvalidateRect(state.native, nullptr, FALSE);
            UpdateWindow(state.native);
        }
        const qreal dpr = state.window->devicePixelRatio();
        const QRect geometry(int(std::floor(logical.x() * dpr)), int(std::floor(logical.y() * dpr)),
            std::max(1, int(std::floor(logical.width() * dpr))), std::max(1, int(std::floor(logical.height() * dpr))));
        if (geometry != state.geometry) {
            SetWindowPos(state.native, HWND_TOP, geometry.x(), geometry.y(), geometry.width(), geometry.height(), SWP_NOACTIVATE);
            state.geometry = geometry;
        }
        state.syncCovers(logical, dpr);
        // The secondary swapchain needs a visible target for its first Present.
        // A black child plus the external preparation caption avoids a readiness
        // loop in which both the UI and the GPU wait for the other to show it.
        if (!IsWindowVisible(state.native)) ShowWindow(state.native, SW_SHOWNOACTIVATE);
    } else if (IsWindowVisible(state.native)) ShowWindow(state.native, SW_HIDE);
    state.bridge.setBrowserPreviewTarget(reinterpret_cast<qulonglong>(state.native), wanted);
    const bool ready = wanted && state.bridge.browserPreview().value("hasFrame").toBool();
    if (state.paint.hasFrame != ready) {
        state.paint.hasFrame = ready;
        if (!ready) InvalidateRect(state.native, nullptr, FALSE);
    }
    state.syncing = false;
}

void BrowserPreviewHost::detach() {
    if (impl_->stopped) return;
    impl_->stopped = true;
    impl_->bridge.setBrowserPreviewTarget(0, false);
    if (impl_->native && IsWindow(impl_->native)) ShowWindow(impl_->native, SW_HIDE);
}
void BrowserPreviewHost::destroy() {
    detach();
    if (impl_->native && IsWindow(impl_->native)) DestroyWindow(impl_->native);
    impl_->native = nullptr;
}
BrowserPreviewHost::~BrowserPreviewHost() { destroy(); }
} // namespace veyra::ui
